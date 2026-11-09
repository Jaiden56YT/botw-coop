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
			case 20:
				x := math.Float32frombits(binary.LittleEndian.Uint32(body[1:]))
				y := math.Float32frombits(binary.LittleEndian.Uint32(body[5:]))
				z := math.Float32frombits(binary.LittleEndian.Uint32(body[9:]))
				_ = y
				if time.Now().Second()%5 == 0 {
					log.Printf("player %d at (%.1f, %.1f, %.1f)", body[0], x, y, z)
				}
			}
		}
	}()

	start := time.Now()
	lastWorld := time.Time{}
	tick := time.NewTicker(33 * time.Millisecond)
	for range tick.C {
		t := time.Since(start).Seconds()
		a := t * 2 * math.Pi / 12 // one lap every 12 s
		st := make([]byte, 44)
		f32(st[0:], c[0]+float32(6*math.Cos(a)))
		f32(st[4:], c[1])
		f32(st[8:], c[2]+float32(6*math.Sin(a)))
		f32(st[12:], float32(-a)) // facing along the circle
		f32(st[16:], float32(-6*math.Sin(a)*2*math.Pi/12))
		f32(st[24:], float32(6*math.Cos(a)*2*math.Pi/12))
		binary.LittleEndian.PutUint16(st[32:], 12)
		binary.LittleEndian.PutUint16(st[34:], 12)
		conn.Write(msg(20, st))
		if time.Since(lastWorld) > time.Second {
			lastWorld = time.Now()
			w := make([]byte, 8)
			f32(w, float32(*clock))
			binary.LittleEndian.PutUint32(w[4:], 7)
			conn.Write(msg(30, w))
			ping := make([]byte, 8)
			conn.Write(msg(4, ping))
		}
	}
}
