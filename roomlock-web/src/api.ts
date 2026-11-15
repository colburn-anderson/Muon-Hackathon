import { fetchAuthSession, getCurrentUser } from 'aws-amplify/auth';

export type RoomState = 'free' | 'held' | 'reserved' | 'warn';

export interface Room {
  room_id: string;
  state: RoomState;
  holder?: string;
  expires_at?: number; // epoch seconds
  next_res_start?: number; // epoch seconds
  yours?: boolean; // real API only: the holder is the signed-in user
}

export interface Reservation {
  res_id: string;
  room_id: string;
  start: number; // epoch seconds
  end: number; // epoch seconds
  user: string;
  checked_in: boolean;
}

export interface ChatMessage {
  role: 'user' | 'assistant';
  content: string;
}

const API_URL: string | undefined = import.meta.env.VITE_API_URL;
export const USING_MOCK = !API_URL;

const MIN = 60;
const now = () => Math.floor(Date.now() / 1000);
const clock = (sec: number) => new Date(sec * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });

export async function currentUserEmail(): Promise<string> {
  const user = await getCurrentUser();
  return user.signInDetails?.loginId ?? user.username;
}

/* ---------- real backend (used once VITE_API_URL is set) ---------- */

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const session = await fetchAuthSession();
  const token = session.tokens?.idToken?.toString();
  const res = await fetch(`${API_URL}${path}`, {
    method,
    headers: {
      'Content-Type': 'application/json',
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  if (!res.ok) throw new Error(text || `${res.status} ${res.statusText}`);
  return (text ? JSON.parse(text) : undefined) as T;
}

// The API's holder is a Cognito username; the UI compares it to the user's email, so use "yours".
const fixRoom = async (r: Room): Promise<Room> => (r.yours ? { ...r, holder: await currentUserEmail() } : r);

const real = {
  getRooms: () => call<{ rooms: Room[] }>('GET', '/rooms').then(({ rooms }) => Promise.all(rooms.map(fixRoom))),
  getRoom: (id: string) => call<Room>('GET', `/rooms/${id}`).then(fixRoom),
  claim: (id: string) => call<Room>('POST', `/rooms/${id}/claim`).then(fixRoom),
  checkIn: (id: string) => call<Room>('POST', `/rooms/${id}/checkin`).then(fixRoom),
  end: (id: string) => call<Room>('POST', `/rooms/${id}/end`).then(fixRoom),
  myReservations: () =>
    call<{ reservations: (Omit<Reservation, 'user' | 'checked_in'> & { status: string })[] }>(
      'GET',
      '/reservations?mine=true',
    ).then(({ reservations }) =>
      reservations.map((r): Reservation => ({ ...r, user: '', checked_in: r.status === 'checked_in' })),
    ),
  reserve: (room_id: string, start: number, end: number) =>
    call<Reservation>('POST', '/reservations', { room_id, start, end }),
  cancel: (res_id: string) => call<void>('DELETE', `/reservations/${res_id}`),
  agent: (message: string, history: ChatMessage[]) =>
    call<{ reply: string }>('POST', '/agent', { message, history }),
};

/* ---------- fake backend (in memory, resets on refresh) ---------- */

const rooms: Room[] = ['1', '2', '3'].map((room_id): Room => ({ room_id, state: 'free' }));
const reservations: Reservation[] = [];

function update(room: Room): Room {
  const t = now();
  if (room.expires_at && room.expires_at <= t) {
    room.holder = undefined;
    room.expires_at = undefined;
  }
  const next = reservations
    .filter((r) => r.room_id === room.room_id && r.end > t && !(r.checked_in && r.user === room.holder))
    .sort((a, b) => a.start - b.start)[0];
  room.next_res_start = next?.start;
  if (room.holder) {
    room.state = room.expires_at && room.expires_at - t <= 5 * MIN ? 'warn' : 'held';
  } else {
    room.state = next && next.start - t <= 10 * MIN ? 'reserved' : 'free';
  }
  return { ...room };
}

function findRoom(id: string): Room {
  const room = rooms.find((r) => r.room_id === id);
  if (!room) throw new Error(`Room ${id} does not exist.`);
  return room;
}

async function mockMine(): Promise<Reservation[]> {
  const user = await currentUserEmail();
  return reservations.filter((r) => r.user === user && r.end > now()).sort((a, b) => a.start - b.start);
}

async function mockReserve(room_id: string, start: number, end: number): Promise<Reservation> {
  findRoom(room_id);
  if (end <= start) throw new Error('End must be after start.');
  if (reservations.some((r) => r.room_id === room_id && r.start < end && start < r.end)) {
    throw new Error('That time overlaps another reservation.');
  }
  const res: Reservation = {
    res_id: crypto.randomUUID(),
    room_id,
    start,
    end,
    user: await currentUserEmail(),
    checked_in: false,
  };
  reservations.push(res);
  return res;
}

// A tiny stand-in for the Bedrock agent so the chat works before the backend exists.
async function mockAgent(message: string, _history: ChatMessage[]): Promise<{ reply: string }> {
  const text = message.toLowerCase();
  const list = rooms.map(update);
  const roomMatch = text.match(/room\s*(\d+)/);

  if (text.includes('book') && roomMatch) {
    const room = list.find((r) => r.room_id === roomMatch[1]);
    if (!room) return { reply: `I couldn't find room ${roomMatch[1]}.` };
    if (room.state !== 'free') return { reply: `Room ${room.room_id} isn't free right now.` };
    const start = now();
    try {
      await mockReserve(room.room_id, start, start + 60 * MIN);
      return {
        reply: `Done. Room ${room.room_id} is yours from ${clock(start)} to ${clock(start + 60 * MIN)}. Scan the QR code on the door to check in.`,
      };
    } catch (e) {
      return { reply: e instanceof Error ? e.message : String(e) };
    }
  }
  if (text.includes('my') && (text.includes('reserv') || text.includes('booking'))) {
    const mine = await mockMine();
    if (mine.length === 0) return { reply: "You don't have any upcoming reservations." };
    return {
      reply: mine.map((r) => `Room ${r.room_id}: ${clock(r.start)} to ${clock(r.end)}`).join('\n'),
    };
  }
  if (text.includes('free') || text.includes('available') || text.includes('open')) {
    const free = list.filter((r) => r.state === 'free');
    if (free.length === 0) return { reply: 'Nothing is free right now. Want me to find the next opening?' };
    return { reply: `Free right now: ${free.map((r) => `Room ${r.room_id}`).join(', ')}.` };
  }
  return {
    reply:
      "I'm running in demo mode, so I only know a few things: ask what's free, say \"book room 2\", or ask for your reservations.",
  };
}

const mock = {
  getRooms: async () => rooms.map(update),
  getRoom: async (id: string) => update(findRoom(id)),
  claim: async (id: string) => {
    const room = findRoom(id);
    if (update(room).state !== 'free') throw new Error('This room is not free right now.');
    room.holder = await currentUserEmail();
    room.expires_at = Math.min(room.next_res_start ?? Infinity, now() + 30 * MIN);
    return update(room);
  },
  checkIn: async (id: string) => {
    const room = findRoom(id);
    const user = await currentUserEmail();
    const t = now();
    const res = reservations.find(
      (r) => r.room_id === id && r.user === user && r.start - 10 * MIN <= t && t < r.end,
    );
    if (!res) throw new Error('You have no reservation for this room right now.');
    res.checked_in = true;
    room.holder = user;
    room.expires_at = res.end;
    return update(room);
  },
  end: async (id: string) => {
    const room = findRoom(id);
    if (room.holder !== (await currentUserEmail())) throw new Error('You are not holding this room.');
    room.holder = undefined;
    room.expires_at = undefined;
    return update(room);
  },
  myReservations: mockMine,
  reserve: mockReserve,
  cancel: async (res_id: string) => {
    const i = reservations.findIndex((r) => r.res_id === res_id);
    if (i >= 0) reservations.splice(i, 1);
  },
  agent: mockAgent,
};

export const api: typeof real = USING_MOCK ? mock : real;