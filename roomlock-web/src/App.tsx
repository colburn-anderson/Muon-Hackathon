import { useCallback, useEffect, useRef, useState, type FormEvent, type ReactNode } from 'react';
import { Routes, Route, Link, NavLink, useParams } from 'react-router-dom';
import { Authenticator } from '@aws-amplify/ui-react';
import { api, USING_MOCK, type ChatMessage, type Reservation, type Room, type RoomState } from './api';
import './App.css';

/* ---------- helpers ---------- */

type Note = { tone: 'ok' | 'error'; text: string };

const msg = (e: unknown) => (e instanceof Error ? e.message : String(e));
const nowSec = () => Math.floor(Date.now() / 1000);
const clock = (sec?: number) =>
  sec ? new Date(sec * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : '';
const day = (sec: number) =>
  new Date(sec * 1000).toLocaleDateString([], { weekday: 'short', month: 'short', day: 'numeric' });

function fromNow(sec?: number): string {
  if (!sec) return '';
  const m = Math.round((sec - nowSec()) / 60);
  if (m <= 0) return 'now';
  if (m < 60) return `in ${m} min`;
  const h = Math.floor(m / 60);
  const rest = m % 60;
  return `in ${h} h${rest ? ` ${rest} min` : ''}`;
}

// Next quarter hour, in the format a datetime-local input expects.
function defaultStart(): string {
  const d = new Date(Date.now() + 15 * 60 * 1000);
  d.setMinutes(Math.ceil(d.getMinutes() / 15) * 15, 0, 0);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

const LABEL: Record<RoomState, string> = {
  free: 'Free',
  held: 'In use',
  reserved: 'Reserved soon',
  warn: 'Ending soon',
};

function describe(room: Room, me: string): string {
  switch (room.state) {
    case 'free':
      return room.next_res_start ? `Free until ${clock(room.next_res_start)}` : 'Free now';
    case 'reserved':
      return `Reserved at ${clock(room.next_res_start)}`;
    case 'held':
    case 'warn':
      return `${room.holder === me ? 'Yours' : 'In use'} until ${clock(room.expires_at)}`;
  }
}

// Runs `load` now and then every 5 seconds.
function usePolling(load: () => Promise<void>) {
  useEffect(() => {
    load();
    const id = setInterval(load, 5000);
    return () => clearInterval(id);
  }, [load]);
}

/* ---------- small pieces ---------- */

// The lock dial fills up as a room gets busier: empty = free, half = reserved soon,
// three quarters = ending soon, full = in use.
function StatusPill({ state }: { state: RoomState }) {
  return (
    <span className="pill">
      <span className={`dial dial-${state}`} />
      {LABEL[state]}
    </span>
  );
}

// A drawn door. It stands ajar when the room is free, swings wide open when the room
// is yours, and stays shut otherwise.
function Door({ room, me }: { room: Room; me: string }) {
  const pose = room.holder === me ? 'open' : room.state === 'free' ? 'ajar' : 'shut';
  return (
    <div className={`door pose-${pose}`} aria-hidden="true">
      <div className="door-frame">
        <div className="door-inside">{pose === 'open' ? 'Yours' : ''}</div>
        <div className="door-leaf">
          <span className="door-number">{room.room_id}</span>
          <span className={`dial dial-${room.state}`} />
        </div>
      </div>
    </div>
  );
}

function Notice({ tone, children }: { tone: Note['tone']; children: ReactNode }) {
  return <div className={`notice ${tone}`}>{children}</div>;
}

function AuthHeader() {
  return (
    <div className="auth-header">
      <div className="brand big">
        <span className="dial dial-warn" />
        RoomLock
      </div>
      <p className="muted">Sign in to unlock your study room.</p>
    </div>
  );
}

/* ---------- pages ---------- */

function RoomsPage({ me }: { me: string }) {
  const [rooms, setRooms] = useState<Room[] | null>(null);
  const [error, setError] = useState('');
  const load = useCallback(async () => {
    try {
      setRooms(await api.getRooms());
      setError('');
    } catch (e) {
      setError(msg(e));
    }
  }, []);
  usePolling(load);

  const free = rooms?.filter((r) => r.state === 'free').length ?? 0;
  return (
    <>
      <div className="page-head">
        <h1>Rooms</h1>
        {rooms && (
          <p className="muted">
            {free} of {rooms.length} free right now
          </p>
        )}
      </div>
      {error && <Notice tone="error">{error}</Notice>}
      {!rooms && !error && <p className="muted">Loading rooms...</p>}
      <div className="room-grid">
        {rooms?.map((r) => (
          <Link key={r.room_id} to={`/r/${r.room_id}`} className="room-tile">
            <div className="door-stand">
              <Door room={r} me={me} />
            </div>
            <span className="room-number">Room {r.room_id}</span>
            <StatusPill state={r.state} />
            <span className="room-tile-detail">{describe(r, me)}</span>
          </Link>
        ))}
      </div>
    </>
  );
}

function ReserveCard({ roomId, onDone }: { roomId: string; onDone: (n: Note) => void }) {
  const [start, setStart] = useState(defaultStart);
  const [minutes, setMinutes] = useState(60);
  const [busy, setBusy] = useState(false);

  async function submit(e: FormEvent) {
    e.preventDefault();
    const s = Math.floor(new Date(start).getTime() / 1000);
    if (!start || Number.isNaN(s)) return onDone({ tone: 'error', text: 'Pick a start time.' });
    setBusy(true);
    try {
      await api.reserve(roomId, s, s + minutes * 60);
      onDone({
        tone: 'ok',
        text: `Booked ${day(s)} at ${clock(s)} for ${minutes} min. Scan the QR code on the door when you arrive.`,
      });
    } catch (err) {
      onDone({ tone: 'error', text: msg(err) });
    } finally {
      setBusy(false);
    }
  }

  return (
    <form className="card" onSubmit={submit}>
      <h2>Reserve this room</h2>
      <label className="field">
        <span>Start</span>
        <input type="datetime-local" value={start} onChange={(e) => setStart(e.target.value)} />
      </label>
      <div className="field">
        <span>Length</span>
        <div className="chips">
          {[30, 60, 90, 120].map((m) => (
            <button
              type="button"
              key={m}
              className={`chip ${m === minutes ? 'on' : ''}`}
              onClick={() => setMinutes(m)}
            >
              {m < 60 ? `${m} min` : `${m / 60} h`}
            </button>
          ))}
        </div>
      </div>
      <button className="btn primary" disabled={busy}>
        {busy ? 'Booking...' : 'Reserve'}
      </button>
    </form>
  );
}

function RoomPage({ me }: { me: string }) {
  const { roomId = '' } = useParams();
  const [room, setRoom] = useState<Room | null>(null);
  const [note, setNote] = useState<Note | null>(null);
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    try {
      setRoom(await api.getRoom(roomId));
    } catch (e) {
      setNote({ tone: 'error', text: msg(e) });
    }
  }, [roomId]);
  usePolling(load);

  async function act(action: () => Promise<unknown>, done: string) {
    setBusy(true);
    try {
      await action();
      setNote({ tone: 'ok', text: done });
      await load();
    } catch (e) {
      setNote({ tone: 'error', text: msg(e) });
    } finally {
      setBusy(false);
    }
  }

  if (!room) return note ? <Notice tone="error">{note.text}</Notice> : <p className="muted">Loading room...</p>;

  const mine = room.holder === me;
  const inUse = room.state === 'held' || room.state === 'warn';
  return (
    <>
      <Link to="/rooms" className="back">
        ← All rooms
      </Link>
      <section className="hero">
        <div className="door-stand hero-door">
          <Door room={room} me={me} />
        </div>
        <h1>Room {room.room_id}</h1>
        <StatusPill state={room.state} />
        <p className="hero-detail">
          {describe(room, me)}
          {inUse && room.expires_at ? ` (ends ${fromNow(room.expires_at)})` : ''}
        </p>
      </section>

      <div className="actions">
        {room.state === 'free' && (
          <button
            className="btn primary big"
            disabled={busy}
            onClick={() => act(() => api.claim(roomId), 'Room claimed. The door is unlocked.')}
          >
            Claim this room
          </button>
        )}
        {room.state === 'reserved' && (
          <button
            className="btn primary big"
            disabled={busy}
            onClick={() => act(() => api.checkIn(roomId), 'Checked in. The door is unlocked.')}
          >
            Check in
          </button>
        )}
        {mine && (
          <button
            className="btn danger big"
            disabled={busy}
            onClick={() => act(() => api.end(roomId), 'Session ended. The door is locked.')}
          >
            I'm done
          </button>
        )}
        {inUse && !mine && <p className="muted center">Someone is using this room. Reserve a time below.</p>}
      </div>

      {note && <Notice tone={note.tone}>{note.text}</Notice>}

      <ReserveCard
        roomId={roomId}
        onDone={(n) => {
          setNote(n);
          load();
        }}
      />
    </>
  );
}

function MyBookingsPage() {
  const [list, setList] = useState<Reservation[] | null>(null);
  const [error, setError] = useState('');
  const load = useCallback(async () => {
    try {
      setList(await api.myReservations());
      setError('');
    } catch (e) {
      setError(msg(e));
    }
  }, []);
  usePolling(load);

  return (
    <>
      <div className="page-head">
        <h1>My bookings</h1>
        <p className="muted">Scan the QR code on the door to check in.</p>
      </div>
      {error && <Notice tone="error">{error}</Notice>}
      {list && list.length === 0 && (
        <div className="card empty">
          <p className="muted">No upcoming bookings.</p>
          <Link to="/" className="btn primary">
            Find a room
          </Link>
        </div>
      )}
      {list?.map((r) => (
        <div key={r.res_id} className="card booking">
          <div className="booking-when">
            <Link to={`/r/${r.room_id}`}>
              <strong>Room {r.room_id}</strong>
            </Link>
            <span className="muted">
              {day(r.start)}, {clock(r.start)} to {clock(r.end)}
              {r.checked_in ? ' (checked in)' : ` (starts ${fromNow(r.start)})`}
            </span>
          </div>
          <button
            className="btn danger small"
            onClick={async () => {
              await api.cancel(r.res_id);
              await load();
            }}
          >
            Cancel
          </button>
        </div>
      ))}
    </>
  );
}

const SUGGESTIONS = [
  'Is anything free right now?',
  'Book room 2 tomorrow at 3pm for an hour',
  'What are my bookings?',
];

function AssistantPage() {
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [input, setInput] = useState('');
  const [busy, setBusy] = useState(false);
  const endRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    endRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages, busy]);

  async function send(text: string) {
    const content = text.trim();
    if (!content || busy) return;
    const history = messages;
    setMessages([...history, { role: 'user', content }]);
    setInput('');
    setBusy(true);
    try {
      const { reply } = await api.agent(content, history);
      setMessages((m) => [...m, { role: 'assistant', content: reply }]);
    } catch (e) {
      setMessages((m) => [...m, { role: 'assistant', content: `Sorry, something went wrong: ${msg(e)}` }]);
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="chat">
      <div className="page-head">
        <h1>Assistant</h1>
        <p className="muted">Ask about the study rooms in plain English: what's free, booking, your bookings, cancelling.</p>
      </div>
      <div className="chat-log">
        <div className="bubble assistant">
          I can check which rooms are free, book one for you, and show or cancel your bookings. What do you need?
        </div>
        {messages.map((m, i) => (
          <div key={i} className={`bubble ${m.role}`}>
            {m.content}
          </div>
        ))}
        {busy && <div className="bubble assistant typing">Thinking...</div>}
        <div ref={endRef} />
      </div>
      {messages.length === 0 && (
        <div className="chips">
          {SUGGESTIONS.map((s) => (
            <button key={s} type="button" className="chip" onClick={() => send(s)}>
              {s}
            </button>
          ))}
        </div>
      )}
      <form
        className="chat-input"
        onSubmit={(e) => {
          e.preventDefault();
          send(input);
        }}
      >
        <input value={input} onChange={(e) => setInput(e.target.value)} placeholder="Ask about rooms..." />
        <button className="btn primary" disabled={busy || !input.trim()}>
          Send
        </button>
      </form>
    </div>
  );
}

/* ---------- app shell ---------- */

export default function App() {
  return (
    <Authenticator components={{ Header: AuthHeader }}>
      {({ signOut, user }) => {
        const me = user?.signInDetails?.loginId ?? user?.username ?? '';
        return (
          <div className="shell">
            <header className="topbar">
              <Link to="/" className="brand">
                <span className="dial dial-warn" />
                RoomLock
              </Link>
              <nav className="nav">
                <NavLink to="/" end>
                  Assistant
                </NavLink>
                <NavLink to="/rooms">Rooms</NavLink>
                <NavLink to="/me">My bookings</NavLink>
              </nav>
              <button className="btn ghost small" onClick={signOut} title={me}>
                Sign out
              </button>
            </header>
            {USING_MOCK && <div className="demo-banner">Demo mode: fake data that resets when you refresh</div>}
            <main className="content">
              <Routes>
                {/* The assistant is the front door; /r/:roomId stays direct because the QR code opens it. */}
                <Route path="/" element={<AssistantPage />} />
                <Route path="/rooms" element={<RoomsPage me={me} />} />
                <Route path="/r/:roomId" element={<RoomPage me={me} />} />
                <Route path="/me" element={<MyBookingsPage />} />
              </Routes>
            </main>
          </div>
        );
      }}
    </Authenticator>
  );
}