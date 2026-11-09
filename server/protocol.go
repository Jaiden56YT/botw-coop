package main

// Wire protocol shared with client/src/net/protocol.hpp. Keep the two in sync.
//
// Every message on the TCP stream is:
//
//	u32 length   (bytes after this field: 4 + len(payload))
//	u16 type
//	u16 flags    (reserved, 0)
//	payload      (little-endian, layout depends on type)
//
// The client sends Hello first. The server answers Welcome or Reject.
// After that, both sides may send anything except Hello/Welcome/Reject.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
)

const (
	ProtocolVersion = 1
	MaxMessageSize  = 64 * 1024
	headerSize      = 4 // type + flags, counted in length
)

type MsgType uint16

const (
	MsgHello        MsgType = 1  // C->S: HelloPayload
	MsgWelcome      MsgType = 2  // S->C: WelcomePayload
	MsgReject       MsgType = 3  // S->C: string reason
	MsgPing         MsgType = 4  // both: u64 timestamp, echoed back as Pong
	MsgPong         MsgType = 5  // both: u64 timestamp
	MsgLog          MsgType = 6  // C->S: string, printed by the server
	MsgPlayerJoined MsgType = 10 // S->C: u8 id, string name
	MsgPlayerLeft   MsgType = 11 // S->C: u8 id
	MsgPlayerState  MsgType = 20 // C->S: PlayerState; S->C: u8 id + PlayerState

	// Game messages (>= MsgGameFirst) are forwarded to everyone else in the
	// room with the sender's id prepended. The server doesn't interpret them.
	MsgGameFirst  MsgType = 30
	MsgWorldState MsgType = 30 // host -> all: f32 WM_Time, s32 WM_NumberOfDays
)

// HelloPayload: u16 protocol, 16 bytes game build id, string name, string password.
type HelloPayload struct {
	Protocol uint16
	BuildID  [16]byte
	Name     string
	Password string
}

// WelcomePayload: u8 your id, u8 host id, u8 count, then count x (u8 id, string name).
type WelcomePayload struct {
	YourID  uint8
	HostID  uint8
	Players []PlayerInfo
}

type PlayerInfo struct {
	ID   uint8
	Name string
}

// PlayerState is the per-frame state of a player's own Link. 44 bytes.
type PlayerState struct {
	Pos      [3]float32 // world position
	RotY     float32    // facing, radians
	Vel      [3]float32 // velocity, for smoothing on the receiving side
	AnimHash uint32     // current animation (0 until M2)
	Health   uint16     // quarter-hearts
	MaxHP    uint16
	MapArea  uint32 // stage / area id, 0 = main field
	Flags    uint32 // bit 0 = gliding, 1 = climbing, 2 = swimming, 3 = mounted
}

const PlayerStateSize = 44

type Message struct {
	Type    MsgType
	Flags   uint16
	Payload []byte
}

var ErrTooLarge = errors.New("message too large")

func ReadMessage(r io.Reader) (Message, error) {
	var lenBuf [4]byte
	if _, err := io.ReadFull(r, lenBuf[:]); err != nil {
		return Message{}, err
	}
	n := binary.LittleEndian.Uint32(lenBuf[:])
	if n < headerSize || n > MaxMessageSize {
		return Message{}, fmt.Errorf("%w: %d", ErrTooLarge, n)
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return Message{}, err
	}
	return Message{
		Type:    MsgType(binary.LittleEndian.Uint16(buf[0:])),
		Flags:   binary.LittleEndian.Uint16(buf[2:]),
		Payload: buf[headerSize:],
	}, nil
}

func EncodeMessage(t MsgType, payload []byte) []byte {
	out := make([]byte, 4+headerSize+len(payload))
	binary.LittleEndian.PutUint32(out[0:], uint32(headerSize+len(payload)))
	binary.LittleEndian.PutUint16(out[4:], uint16(t))
	copy(out[8:], payload)
	return out
}

// --- payload helpers -------------------------------------------------------

type writer struct{ bytes.Buffer }

func (w *writer) u8(v uint8)   { w.WriteByte(v) }
func (w *writer) u16(v uint16) { binary.Write(&w.Buffer, binary.LittleEndian, v) }
func (w *writer) u32(v uint32) { binary.Write(&w.Buffer, binary.LittleEndian, v) }
func (w *writer) f32(v float32) {
	w.u32(math.Float32bits(v))
}

// Strings are u16 length + UTF-8 bytes.
func (w *writer) str(s string) {
	if len(s) > 0xFFFF {
		s = s[:0xFFFF]
	}
	w.u16(uint16(len(s)))
	w.WriteString(s)
}

type reader struct {
	b   []byte
	err error
}

func (r *reader) take(n int) []byte {
	if r.err != nil {
		return nil
	}
	if len(r.b) < n {
		r.err = io.ErrUnexpectedEOF
		return nil
	}
	out := r.b[:n]
	r.b = r.b[n:]
	return out
}

func (r *reader) u8() uint8 {
	if b := r.take(1); b != nil {
		return b[0]
	}
	return 0
}
func (r *reader) u16() uint16 {
	if b := r.take(2); b != nil {
		return binary.LittleEndian.Uint16(b)
	}
	return 0
}
func (r *reader) u32() uint32 {
	if b := r.take(4); b != nil {
		return binary.LittleEndian.Uint32(b)
	}
	return 0
}
func (r *reader) f32() float32 { return math.Float32frombits(r.u32()) }
func (r *reader) str() string {
	n := int(r.u16())
	return string(r.take(n))
}

func DecodeHello(p []byte) (HelloPayload, error) {
	r := reader{b: p}
	var h HelloPayload
	h.Protocol = r.u16()
	copy(h.BuildID[:], r.take(16))
	h.Name = r.str()
	h.Password = r.str()
	return h, r.err
}

func EncodeHello(h HelloPayload) []byte {
	var w writer
	w.u16(h.Protocol)
	w.Write(h.BuildID[:])
	w.str(h.Name)
	w.str(h.Password)
	return w.Bytes()
}

func EncodeWelcome(wp WelcomePayload) []byte {
	var w writer
	w.u8(wp.YourID)
	w.u8(wp.HostID)
	w.u8(uint8(len(wp.Players)))
	for _, p := range wp.Players {
		w.u8(p.ID)
		w.str(p.Name)
	}
	return w.Bytes()
}

func DecodeWelcome(p []byte) (WelcomePayload, error) {
	r := reader{b: p}
	var wp WelcomePayload
	wp.YourID = r.u8()
	wp.HostID = r.u8()
	n := int(r.u8())
	for i := 0; i < n && r.err == nil; i++ {
		wp.Players = append(wp.Players, PlayerInfo{ID: r.u8(), Name: r.str()})
	}
	return wp, r.err
}

func EncodeString(s string) []byte {
	var w writer
	w.str(s)
	return w.Bytes()
}

func DecodeString(p []byte) (string, error) {
	r := reader{b: p}
	s := r.str()
	return s, r.err
}

func EncodePlayerState(s PlayerState) []byte {
	var w writer
	for _, v := range s.Pos {
		w.f32(v)
	}
	w.f32(s.RotY)
	for _, v := range s.Vel {
		w.f32(v)
	}
	w.u32(s.AnimHash)
	w.u16(s.Health)
	w.u16(s.MaxHP)
	w.u32(s.MapArea)
	w.u32(s.Flags)
	return w.Bytes()
}

func DecodePlayerState(p []byte) (PlayerState, error) {
	if len(p) != PlayerStateSize {
		return PlayerState{}, fmt.Errorf("player state: want %d bytes, got %d", PlayerStateSize, len(p))
	}
	r := reader{b: p}
	var s PlayerState
	for i := range s.Pos {
		s.Pos[i] = r.f32()
	}
	s.RotY = r.f32()
	for i := range s.Vel {
		s.Vel[i] = r.f32()
	}
	s.AnimHash = r.u32()
	s.Health = r.u16()
	s.MaxHP = r.u16()
	s.MapArea = r.u32()
	s.Flags = r.u32()
	return s, r.err
}
