package main

import (
	"bufio"
	"net"
	"strings"
	"testing"
	"time"
)

var build = [16]byte{0xcd, 0x57, 0xb2, 0x3f}

func startServer(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := NewServer()
	s.logf = t.Logf
	go s.Serve(ln)
	t.Cleanup(func() { ln.Close() })
	return ln.Addr().String()
}

type testClient struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

func dial(t *testing.T, addr string, h HelloPayload) *testClient {
	t.Helper()
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	conn.Write(EncodeMessage(MsgHello, EncodeHello(h)))
	return &testClient{t: t, conn: conn, r: bufio.NewReader(conn)}
}

func (c *testClient) next() Message {
	c.t.Helper()
	c.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	m, err := ReadMessage(c.r)
	if err != nil {
		c.t.Fatalf("read: %v", err)
	}
	return m
}

func (c *testClient) expect(t MsgType) Message {
	c.t.Helper()
	m := c.next()
	if m.Type != t {
		c.t.Fatalf("got message type %d, want %d", m.Type, t)
	}
	return m
}

func hello(name, pw string) HelloPayload {
	return HelloPayload{Protocol: ProtocolVersion, BuildID: build, Name: name, Password: pw}
}

func TestTwoPlayersSeeEachOther(t *testing.T) {
	addr := startServer(t)

	a := dial(t, addr, hello("Link", "hyrule"))
	wa, _ := DecodeWelcome(a.expect(MsgWelcome).Payload)
	if wa.YourID != 1 || wa.HostID != 1 || len(wa.Players) != 0 {
		t.Fatalf("host welcome = %+v", wa)
	}

	b := dial(t, addr, hello("Sidon", "hyrule"))
	wb, _ := DecodeWelcome(b.expect(MsgWelcome).Payload)
	if wb.YourID != 2 || wb.HostID != 1 || len(wb.Players) != 1 || wb.Players[0].Name != "Link" {
		t.Fatalf("guest welcome = %+v", wb)
	}
	a.expect(MsgPlayerJoined)

	state := PlayerState{Pos: [3]float32{-1060, 250, 1800}, RotY: 1.5, Health: 12, MaxHP: 12}
	a.conn.Write(EncodeMessage(MsgPlayerState, EncodePlayerState(state)))

	got := b.expect(MsgPlayerState)
	if got.Payload[0] != 1 {
		t.Fatalf("state from id %d, want 1", got.Payload[0])
	}
	decoded, err := DecodePlayerState(got.Payload[1:])
	if err != nil || decoded != state {
		t.Fatalf("state = %+v, %v; want %+v", decoded, err, state)
	}
}

func TestRoomsAreSeparatedByPassword(t *testing.T) {
	addr := startServer(t)
	a := dial(t, addr, hello("Link", "one"))
	a.expect(MsgWelcome)
	b := dial(t, addr, hello("Zelda", "two"))
	wb, _ := DecodeWelcome(b.expect(MsgWelcome).Payload)
	if wb.HostID != wb.YourID || len(wb.Players) != 0 {
		t.Fatalf("second room should be empty with Zelda as host: %+v", wb)
	}
}

func TestRejections(t *testing.T) {
	addr := startServer(t)
	host := dial(t, addr, hello("Link", "pw"))
	host.expect(MsgWelcome)

	cases := map[string]HelloPayload{
		"protocol": {Protocol: 99, BuildID: build, Name: "A", Password: "pw"},
		"version":  {Protocol: ProtocolVersion, BuildID: [16]byte{1}, Name: "B", Password: "pw"},
		"name":     hello("Link", "pw"),
	}
	for want, h := range cases {
		c := dial(t, addr, h)
		reason, _ := DecodeString(c.expect(MsgReject).Payload)
		if !strings.Contains(reason, want) {
			t.Errorf("%s: reject reason %q", want, reason)
		}
	}
}

func TestHostLeavingHandsOverInsteadOfEndingSession(t *testing.T) {
	addr := startServer(t)
	a := dial(t, addr, hello("Link", "pw"))
	a.expect(MsgWelcome)
	b := dial(t, addr, hello("Mipha", "pw"))
	b.expect(MsgWelcome)
	a.expect(MsgPlayerJoined)

	a.conn.Close()
	left := b.expect(MsgPlayerLeft)
	if left.Payload[0] != 1 {
		t.Fatalf("left id %d", left.Payload[0])
	}
	hc := b.expect(MsgHostChanged)
	if hc.Payload[0] != 2 {
		t.Fatalf("new host %d, want 2", hc.Payload[0])
	}
}

func TestPingIsAnswered(t *testing.T) {
	addr := startServer(t)
	a := dial(t, addr, hello("Link", "pw"))
	a.expect(MsgWelcome)
	a.conn.Write(EncodeMessage(MsgPing, putU64(42)))
	if p := a.expect(MsgPong); string(p.Payload) != string(putU64(42)) {
		t.Fatalf("pong payload %x", p.Payload)
	}
}
