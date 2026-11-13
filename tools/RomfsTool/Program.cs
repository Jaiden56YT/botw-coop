// Reads BotW's RomFS with an update applied, the same way Ryujinx does.
//
//   dotnet run --project tools/RomfsTool -- <base.nsp> <update.nsp> list [substring]
//   dotnet run --project tools/RomfsTool -- <base.nsp> <update.nsp> extract <romfs path> <out file>
//   dotnet run --project tools/RomfsTool -- <base.nsp> <update.nsp> extract-all <out dir> [path prefix]
//       the whole RomFS (update applied), folders kept: what Savage Wind Recomp reads as extracted/romfs
//   dotnet run --project tools/RomfsTool -- <base.nsp> <update.nsp> exefs <out dir>
//       the update's ExeFS (main, main.npdm, rtld, sdk, subsdk0): extracted/exefs
//
// Keys come from ~/.switch/prod.keys (and title.keys if present).

using LibHac;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Es;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using Path = System.IO.Path;

if (args.Length < 3)
{
    Console.Error.WriteLine("usage: <base.nsp> <update.nsp> list [substring] | extract <romfs path> <out file>");
    return 2;
}

string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
string prodKeys = Path.Combine(home, ".switch", "prod.keys");
string titleKeys = Path.Combine(home, ".switch", "title.keys");

KeySet keySet = new();
ExternalKeyReader.ReadKeyFile(keySet, prodKeys, null, File.Exists(titleKeys) ? titleKeys : null, null, null);

Nca baseNca = OpenProgramNca(args[0], keySet);
Nca patchNca = OpenProgramNca(args[1], keySet);

IStorage romfsStorage = baseNca.OpenStorageWithPatch(patchNca, NcaSectionType.Data, IntegrityCheckLevel.None);
RomFsFileSystem romfs = new(romfsStorage);

switch (args[2])
{
    case "list":
    {
        string filter = args.Length > 3 ? args[3] : "";
        foreach (DirectoryEntryEx entry in romfs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        {
            if (entry.Type == DirectoryEntryType.File && entry.FullPath.Contains(filter))
                Console.WriteLine($"{entry.Size} {entry.FullPath}");
        }
        return 0;
    }
    case "extract":
    {
        string path = args[3].StartsWith('/') ? args[3] : "/" + args[3];
        using UniqueRef<IFile> file = new();
        romfs.OpenFile(ref file.Ref, path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        file.Get.GetSize(out long size).ThrowIfFailure();
        byte[] data = new byte[size];
        file.Get.Read(out long read, 0, data).ThrowIfFailure();
        File.WriteAllBytes(args[4], data);
        Console.WriteLine($"wrote {read} bytes to {args[4]}");
        return 0;
    }
    case "extract-prefix":
    {
        // Every file whose path starts with args[3], written flat into directory args[4].
        string prefix = args[3].StartsWith('/') ? args[3] : "/" + args[3];
        Directory.CreateDirectory(args[4]);
        int n = 0;
        foreach (DirectoryEntryEx entry in romfs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        {
            if (entry.Type != DirectoryEntryType.File || !entry.FullPath.StartsWith(prefix)) continue;
            using UniqueRef<IFile> file = new();
            romfs.OpenFile(ref file.Ref, entry.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            file.Get.GetSize(out long size).ThrowIfFailure();
            byte[] data = new byte[size];
            file.Get.Read(out long _, 0, data).ThrowIfFailure();
            File.WriteAllBytes(Path.Combine(args[4], Path.GetFileName(entry.FullPath)), data);
            n++;
        }
        Console.WriteLine($"wrote {n} files to {args[4]}");
        return 0;
    }
    case "extract-all":
    {
        string prefix = args.Length > 4 ? (args[4].StartsWith('/') ? args[4] : "/" + args[4]) : "/";
        int n = 0;
        long bytes = 0;
        var started = DateTime.Now;
        foreach (DirectoryEntryEx entry in romfs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        {
            if (entry.Type != DirectoryEntryType.File || !entry.FullPath.StartsWith(prefix)) continue;
            string dest = Path.Combine(args[3], entry.FullPath.TrimStart('/').Replace('/', Path.DirectorySeparatorChar));
            CopyOut(romfs, entry.FullPath, dest, entry.Size);
            n++;
            bytes += entry.Size;
            if (n % 2000 == 0) Console.WriteLine($"  {n} files, {bytes >> 20} MB");
        }
        Console.WriteLine($"wrote {n} files ({bytes >> 20} MB) to {args[3]} in {(DateTime.Now - started).TotalSeconds:F0} s");
        return 0;
    }
    case "exefs":
    {
        // An update carries the whole ExeFS of its version (the code isn't patched like the RomFS).
        using UniqueRef<IFileSystem> exefs = new();
        exefs.Reset(patchNca.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.None));
        int n = 0;
        foreach (DirectoryEntryEx entry in exefs.Get.EnumerateEntries("/", "*"))
        {
            if (entry.Type != DirectoryEntryType.File) continue;
            CopyOut(exefs.Get, entry.FullPath, Path.Combine(args[3], entry.Name), entry.Size);
            n++;
        }
        Console.WriteLine($"wrote {n} files to {args[3]}");
        return 0;
    }
    default:
        Console.Error.WriteLine($"unknown command {args[2]}");
        return 2;
}

static void CopyOut(IFileSystem fs, string path, string dest, long size)
{
    Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
    using UniqueRef<IFile> file = new();
    fs.OpenFile(ref file.Ref, path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
    using FileStream output = File.Create(dest);
    byte[] buf = new byte[4 << 20];
    for (long at = 0; at < size;)
    {
        file.Get.Read(out long got, at, buf.AsSpan(0, (int)Math.Min(buf.Length, size - at))).ThrowIfFailure();
        if (got <= 0) throw new IOException($"{path}: read stopped at {at} of {size}");
        output.Write(buf, 0, (int)got);
        at += got;
    }
}

static Nca OpenProgramNca(string nspPath, KeySet keySet)
{
    LocalStorage storage = new(nspPath, FileAccess.Read, FileMode.Open);
    PartitionFileSystem pfs = new();
    pfs.Initialize(storage).ThrowIfFailure();

    // Title keys from the NSP's tickets, as Ryujinx does when it opens an NSP.
    foreach (DirectoryEntryEx ticketEntry in pfs.EnumerateEntries("/", "*.tik"))
    {
        using UniqueRef<IFile> ticketFile = new();
        if (pfs.OpenFile(ref ticketFile.Ref, ticketEntry.FullPath.ToU8Span(), OpenMode.Read).IsSuccess())
        {
            Ticket ticket = new(ticketFile.Get.AsStream());
            byte[] titleKey = ticket.GetTitleKey(keySet);
            if (titleKey != null)
                keySet.ExternalKeySet.Add(new RightsId(ticket.RightsId), new AccessKey(titleKey));
        }
    }

    foreach (DirectoryEntryEx entry in pfs.EnumerateEntries("/", "*.nca"))
    {
        UniqueRef<IFile> ncaFile = new();
        pfs.OpenFile(ref ncaFile.Ref, entry.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        Nca nca = new(keySet, ncaFile.Release().AsStorage());
        if (nca.Header.ContentType == NcaContentType.Program)
            return nca;
    }
    throw new InvalidOperationException($"no program NCA in {nspPath}");
}
