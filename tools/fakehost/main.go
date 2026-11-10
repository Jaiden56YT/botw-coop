// Command fakehost joins a relay room as the host and plays a scripted
// player, so one real game can be tested as a guest:
//
//	go run ./tools/fakehost -addr 127.0.0.1:7777 -password test -center -1060,250,1830
//
// It sends the world clock (fixed time, default 12:00) every second and a
// player walking a 6 m circle around -center at 30 Hz.
package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"hash/crc32"
	"fmt"
	"io"
	"log"
	"math"
	"net"
	"strconv"
	"strings"
	"time"
)

func msg(t uint16, payload []byte) []byte {
	out := make([]byte, 8+len(payload))
	binary.LittleEndian.PutUint32(out, uint32(4+len(payload)))
	binary.LittleEndian.PutUint16(out[4:], t)
	copy(out[8:], payload)
	return out
}

func str(s string) []byte {
	b := make([]byte, 2+len(s))
	binary.LittleEndian.PutUint16(b, uint16(len(s)))
	copy(b[2:], s)
	return b
}

func f32(b []byte, v float32) { binary.LittleEndian.PutUint32(b, math.Float32bits(v)) }

func main() {
	addr := flag.String("addr", "127.0.0.1:7777", "relay address")
	pw := flag.String("password", "test", "room password")
	name := flag.String("name", "FakeHost", "player name")
	center := flag.String("center", "-1060,250,1830", "x,y,z the fake player circles")
	clock := flag.Float64("time", 180, "WM_Time to send (degrees, 15 per hour; 180 = 12:00)")
	buildHex := flag.String("build", "cd57b23fa4bbad65803d9788c01821ee", "game build id")
	tour := flag.Bool("tour", false, "cycle stand / walk / run / sprint every 6 s instead of a steady jog")
	flagList := flag.String("flags", "", "comma-separated progress flags to share 25 s after joining (e.g. MapTower_01)")
	shrineAt := flag.Int("shrine-at", 0, "if > 0: after this many seconds, report being inside a shrine (CDungeon/Dungeon000) for 20 s, then back outside")
	enemy := flag.String("enemy", "", "HASH:LIFE (hex hash): 15 s after joining, report that this overworld enemy now has LIFE")
	weather := flag.Int("weather", -1, "weather to host (0 Bluesky, 1 Cloudy, 2 Rain, 3 HeavyRain, 4 Snow, 5 HeavySnow, 6 ThunderStorm, 7 ThunderRain, 8 BlueskyRain); -1 = none")
	climate := flag.Int("climate", 0, "the host's climate number (weather is only shared within one climate)")
	actions := flag.Bool("actions", false, "cycle the reported action (attack 1-3, finisher, spin, charge, bow aim, bow shoot, throw aim, throw, guard) every 3 s while standing")
	gear := flag.String("gear", "", "HEAD,UPPER,LOWER armor actor names the fake player wears (e.g. Armor_001_Head,Armor_151_Upper,Armor_155_Lower), sent every 5 s")
	still := flag.Bool("still", false, "stand still (at center + 6 m on x)")
	anims := flag.Bool("anims", false, "cycle the reported movement state (stand, move, climb, swim, glide, horse, surf) every 5 s")
	flag.Parse()

	var c [3]float32
	for i, p := range strings.Split(*center, ",") {
		v, err := strconv.ParseFloat(strings.TrimSpace(p), 32)
		if err != nil {
			log.Fatalf("bad -center: %v", err)
		}
		c[i] = float32(v)
	}

	conn, err := net.Dial("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	hello := []byte{1, 0}
	var build [16]byte
	for i := 0; i < 16; i++ {
		fmt.Sscanf((*buildHex)[2*i:2*i+2], "%02x", &build[i])
	}
	hello = append(hello, build[:]...)
	hello = append(hello, str(*name)...)
	hello = append(hello, str(*pw)...)
	conn.Write(msg(1, hello))

	go func() {
		r := bufio.NewReader(conn)
		for {
			var hdr [8]byte
			if _, err := io.ReadFull(r, hdr[:]); err != nil {
				log.Fatalf("disconnected: %v", err)
			}
			n := binary.LittleEndian.Uint32(hdr[:]) - 4
			t := binary.LittleEndian.Uint16(hdr[4:])
			body := make([]byte, n)
			io.ReadFull(r, body)
			switch t {
			case 2:
				log.Printf("joined as player %d, host %d", body[0], body[1])
			case 3:
				log.Fatalf("rejected: %s", body[2:])
			case 10:
				log.Printf("%s joined", body[3:])
			case 31:
				log.Printf("player %d shared flag %s", body[0], body[4:])
			case 33:
				log.Printf("player %d went down and was revived (partner nearby)", body[0])
			case 32:
				log.Printf("player %d: enemy %08x in area %08x now has life %d", body[0],
					binary.LittleEndian.Uint32(body[5:]), binary.LittleEndian.Uint32(body[1:]),
					int32(binary.LittleEndian.Uint32(body[9:])))
			case 20:
				x := math.Float32frombits(binary.LittleEndian.Uint32(body[1:]))
				y := math.Float32frombits(binary.LittleEndian.Uint32(body[5:]))
				z := math.Float32frombits(binary.LittleEndian.Uint32(body[9:]))
				_ = y
				if time.Now().Second()%5 == 0 {
					anim := binary.LittleEndian.Uint32(body[1+28:])
					log.Printf("player %d at (%.1f, %.1f, %.1f) anim %d", body[0], x, y, z, anim)
				}
			}
		}
	}()

	start := time.Now()
	lastWorld := time.Time{}
	lastGear := time.Time{}
	flagsSent := *flagList == ""
	enemySent := *enemy == ""
	tick := time.NewTicker(33 * time.Millisecond)
	const radius = 6.0
	a := 0.0
	last := time.Now()
	for range tick.C {
		now := time.Now()
		dt := now.Sub(last).Seconds()
		last = now
		speed := radius * 2 * math.Pi / 12 // steady jog: one lap every 12 s (~3.1 m/s)
		if *tour {
			speed = []float64{0, 1.5, 4.5, 8}[int(time.Since(start).Seconds()/6)%4]
		}
		if *actions || *still {
			speed = 0 // stand still so the actions are easy to see
		}
		a += speed / radius * dt
		st := make([]byte, 44)
		f32(st[0:], c[0]+float32(radius*math.Cos(a)))
		f32(st[4:], c[1])
		f32(st[8:], c[2]+float32(radius*math.Sin(a)))
		// Facing along the circle (yaw 0 = +Z, measured like the mod: atan2(x, z)).
		f32(st[12:], float32(math.Atan2(-math.Sin(a), math.Cos(a))))
		f32(st[16:], float32(-speed*math.Sin(a)))
		f32(st[24:], float32(speed*math.Cos(a)))
		if *actions {
			// Standing (1) with anim::Action in bits 8-15, 1..11.
			act := uint32(int(time.Since(start).Seconds()/3)%11 + 1)
			binary.LittleEndian.PutUint32(st[28:], 1|act<<8)
			f32(st[16:], 0)
			f32(st[24:], 0)
		}
		if *anims {
			// anim::Category values: 1 Wait, 2 Move, 3 Climb, 5 Swim, 8 Glide, 11 Horse, 12 Surf
			cycle := []uint32{1, 2, 3, 5, 8, 11, 12}
			binary.LittleEndian.PutUint32(st[28:], cycle[int(time.Since(start).Seconds()/5)%len(cycle)])
		}
		// map_area: the mod's area id, CRC32 of the stage type (+ name off the overworld).
		area := crc32.ChecksumIEEE([]byte("MainField"))
		if t := int(time.Since(start).Seconds()); *shrineAt > 0 && t >= *shrineAt && t < *shrineAt+20 {
			area = crc32.ChecksumIEEE([]byte("CDungeonDungeon000"))
		}
		binary.LittleEndian.PutUint32(st[36:], area)
		binary.LittleEndian.PutUint16(st[32:], 12)
		binary.LittleEndian.PutUint16(st[34:], 12)
		conn.Write(msg(20, st))
		if *gear != "" && time.Since(lastGear) > 5*time.Second {
			lastGear = time.Now()
			parts := strings.SplitN(*gear+",,", ",", 4)
			var p []byte
			for _, part := range parts[:3] {
				b := make([]byte, 2)
				binary.LittleEndian.PutUint16(b, uint16(len(part)))
				p = append(p, b...)
				p = append(p, part...)
			}
			conn.Write(msg(34, p))
		}
		if !enemySent && time.Since(start) > 15*time.Second {
			enemySent = true
			var h uint32
			var life int32
			if _, err := fmt.Sscanf(*enemy, "%x:%d", &h, &life); err != nil {
				log.Fatalf("-enemy: %v", err)
			}
			p := make([]byte, 12)
			binary.LittleEndian.PutUint32(p[0:], crc32.ChecksumIEEE([]byte("MainField")))
			binary.LittleEndian.PutUint32(p[4:], h)
			binary.LittleEndian.PutUint32(p[8:], uint32(life))
			conn.Write(msg(32, p))
			log.Printf("sent: enemy %08x life %d", h, life)
		}
		if !flagsSent && time.Since(start) > 25*time.Second {
			flagsSent = true
			for _, name := range strings.Split(*flagList, ",") {
				name = strings.TrimSpace(name)
				p := append([]byte{1}, str(name)...)
				conn.Write(msg(31, p))
				log.Printf("shared flag %s", name)
			}
		}
		if time.Since(lastWorld) > time.Second {
			lastWorld = time.Now()
			w := make([]byte, 8, 10)
			f32(w, float32(*clock))
			binary.LittleEndian.PutUint32(w[4:], 7)
			if *weather >= 0 {
				w = append(w, byte(*weather), byte(*climate))
			}
			conn.Write(msg(30, w))
			ping := make([]byte, 8)
			conn.Write(msg(4, ping))
		}
	}
}
