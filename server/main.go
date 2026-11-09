// Command server is the botw-coop relay. Players connect over TCP, join a room
// by password, and the server forwards their state to everyone else in it.
//
//	go run . -listen :7777
package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"sync"
	"time"
)

const (
	MaxPlayersPerRoom = 4
	readTimeout       = 15 * time.Second // clients ping every 2 s
	handshakeTimeout  = 5 * time.Second
	sendQueueSize     = 256
)

const MsgHostChanged MsgType = 12 // S->C: u8 new host id

type Server struct {
	mu    sync.Mutex
	rooms map[string]*Room
	logf  func(format string, args ...any)
}

type Room struct {
	password string
	buildID  [16]byte
	hostID   uint8
	players  map[uint8]*Client
}

type Client struct {
	conn net.Conn
	id   uint8
	name string
	room *Room
	send chan []byte

	mu     sync.Mutex // guards closed and sends on send
	closed bool
}

func NewServer() *Server {
	return &Server{rooms: map[string]*Room{}, logf: log.Printf}
}

func main() {
	listen := flag.String("listen", ":7777", "address to listen on")
	flag.Parse()

	ln, err := net.Listen("tcp", *listen)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("botw-coop relay listening on %s (protocol %d)", ln.Addr(), ProtocolVersion)
	NewServer().Serve(ln)
}

func (s *Server) Serve(ln net.Listener) error {
	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		if tc, ok := conn.(*net.TCPConn); ok {
			tc.SetNoDelay(true)
		}
		go s.handle(conn)
	}
}

func (s *Server) handle(conn net.Conn) {
	defer conn.Close()
	br := bufio.NewReader(conn)

	conn.SetReadDeadline(time.Now().Add(handshakeTimeout))
	msg, err := ReadMessage(br)
	if err != nil || msg.Type != MsgHello {
		s.logf("%s: no hello (%v)", conn.RemoteAddr(), err)
		return
	}
	hello, err := DecodeHello(msg.Payload)
	if err != nil {
		s.logf("%s: bad hello: %v", conn.RemoteAddr(), err)
		return
	}

	c := &Client{conn: conn, name: hello.Name, send: make(chan []byte, sendQueueSize)}
	if reason := s.join(c, hello); reason != "" {
		s.logf("%s (%s) rejected: %s", conn.RemoteAddr(), hello.Name, reason)
		conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
		conn.Write(EncodeMessage(MsgReject, EncodeString(reason)))
		return
	}
	go c.writeLoop()
	defer s.leave(c)

	for {
		conn.SetReadDeadline(time.Now().Add(readTimeout))
		msg, err := ReadMessage(br)
		if err != nil {
			if !errors.Is(err, io.EOF) {
				s.logf("%s: read: %v", c.name, err)
			}
			return
		}
		s.dispatch(c, msg)
	}
}

func (s *Server) join(c *Client, h HelloPayload) string {
	if h.Protocol != ProtocolVersion {
		return fmt.Sprintf("protocol mismatch: server %d, you %d; update the mod", ProtocolVersion, h.Protocol)
	}
	if h.Name == "" {
		return "empty player name"
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	room := s.rooms[h.Password]
	if room == nil {
		room = &Room{password: h.Password, buildID: h.BuildID, players: map[uint8]*Client{}}
		s.rooms[h.Password] = room
	}
	if room.buildID != h.BuildID {
		return "game version differs from the host's; everyone needs the same BotW update"
	}
	if len(room.players) >= MaxPlayersPerRoom {
		return "session is full"
	}
	for _, p := range room.players {
		if p.name == h.Name {
			return "a player with that name is already in the session"
		}
	}

	id := uint8(1)
	for room.players[id] != nil {
		id++
	}
	c.id, c.room = id, room
	if len(room.players) == 0 {
		room.hostID = id
	}

	welcome := WelcomePayload{YourID: id, HostID: room.hostID}
	for _, p := range room.players {
		welcome.Players = append(welcome.Players, PlayerInfo{ID: p.id, Name: p.name})
		p.queue(EncodeMessage(MsgPlayerJoined, joinedPayload(c)), true)
	}
	room.players[id] = c
	c.queue(EncodeMessage(MsgWelcome, EncodeWelcome(welcome)), true)

	role := "guest"
	if id == room.hostID {
		role = "host"
	}
	s.logf("%s joined as player %d (%s), %d in session", c.name, id, role, len(room.players))
	return ""
}

func (s *Server) leave(c *Client) {
	s.mu.Lock()
	defer s.mu.Unlock()

	room := c.room
	delete(room.players, c.id)
	c.close()
	s.logf("%s left, %d in session", c.name, len(room.players))

	if len(room.players) == 0 {
		delete(s.rooms, room.password)
		return
	}
	for _, p := range room.players {
		p.queue(EncodeMessage(MsgPlayerLeft, []byte{c.id}), true)
	}
	if room.hostID == c.id {
		// The lowest remaining id becomes host. The session keeps going.
		next := uint8(255)
		for id := range room.players {
			if id < next {
				next = id
			}
		}
		room.hostID = next
		for _, p := range room.players {
			p.queue(EncodeMessage(MsgHostChanged, []byte{next}), true)
		}
		s.logf("%s is now the host", room.players[next].name)
	}
}

func (s *Server) dispatch(c *Client, msg Message) {
	switch msg.Type {
	case MsgPing:
		c.queue(EncodeMessage(MsgPong, msg.Payload), false)
	case MsgPong:
		// Client answered our ping; nothing to do yet.
	case MsgLog:
		text, _ := DecodeString(msg.Payload)
		s.logf("[%s] %s", c.name, text)
	case MsgPlayerState:
		if len(msg.Payload) != PlayerStateSize {
			s.logf("%s: bad player state size %d", c.name, len(msg.Payload))
			return
		}
		out := EncodeMessage(MsgPlayerState, append([]byte{c.id}, msg.Payload...))
		s.broadcast(c, out, false)
	default:
		s.logf("%s: unknown message type %d", c.name, msg.Type)
	}
}

// broadcast sends to everyone in c's room except c.
func (s *Server) broadcast(from *Client, data []byte, reliable bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, p := range from.room.players {
		if id != from.id {
			p.queue(data, reliable)
		}
	}
}

// queue hands data to the client's writer. Unreliable messages (per-frame
// state) are dropped if the client is falling behind; a newer one follows
// shortly. If a reliable message can't be queued the client is too far
// behind to recover, so it is disconnected.
func (c *Client) queue(data []byte, reliable bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return
	}
	select {
	case c.send <- data:
	default:
		if reliable {
			c.closeLocked()
		}
	}
}

func (c *Client) writeLoop() {
	w := bufio.NewWriter(c.conn)
	for data := range c.send {
		c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if _, err := w.Write(data); err != nil {
			c.conn.Close()
			return
		}
		// Coalesce whatever else is already queued into one write.
		for len(c.send) > 0 {
			if _, err := w.Write(<-c.send); err != nil {
				c.conn.Close()
				return
			}
		}
		if err := w.Flush(); err != nil {
			c.conn.Close()
			return
		}
	}
}

func (c *Client) close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.closeLocked()
}

func (c *Client) closeLocked() {
	if c.closed {
		return
	}
	c.closed = true
	close(c.send)
	c.conn.Close()
}

func joinedPayload(c *Client) []byte {
	var w writer
	w.u8(c.id)
	w.str(c.name)
	return w.Bytes()
}

// putU64 is used by tests and future server-initiated pings.
func putU64(v uint64) []byte {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint64(b, v)
	return b
}
