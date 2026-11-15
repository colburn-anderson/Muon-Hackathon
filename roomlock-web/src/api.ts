import { fetchAuthSession, getCurrentUser } from 'aws-amplify/auth';

/* Shapes match Keegan's "Room Lock API — Team Guide". */

export type RoomState = 'free' | 'held' | 'reserved' | 'warn';

export interface Room {
  room_id: string;
  state: RoomState;
  presence?: number | boolean | null; // 1 when the room's sensor sees someone inside
  expires_at?: number | null; // epoch seconds, null when not held
  next_res_start?: number | null; // epoch seconds, null when nothing is booked
  holder?: string; // Cognito username, only while held
  yours?: boolean; // true when you are the one holding the room
  kind?: 'soft' | 'hard'; // walk-up session or checked-in booking
  slot_seconds?: number; // booking slot length; admins can change it
}

export interface Reservation {
  res_id: string;
  room_id: string;
  start: number; // epoch seconds
  end: number; // epoch seconds
  status?: string;
}

export interface AdminSession {
  res_id: string;
  user: string; // Cognito username
  kind: 'soft' | 'hard'; // walk-up session or checked-in booking
  started_at: number; // epoch seconds
  expires_at: number; // epoch seconds, the next extension point
}

export interface AdminRoom {
  room_id: string;
  presence: boolean;
  session: AdminSession | null;
  reservations: { sk: string; user?: string; start: number; end: number; status?: string }[]; // newest first
  stray_slots: { sk: string; start: number; res_id: string }[]; // blocking slots nothing owns
}

export interface AdminView {
  seconds_per_minute: number; // 60 = real time
  slot_minutes: number; // booking slot length
  rules: Record<string, number>; // each rule's current length in seconds
  rooms: AdminRoom[];
}

export interface ChatMessage {
  role: 'user' | 'assistant';
  content: string;
}

const API_URL: string | undefined = import.meta.env.VITE_API_URL?.replace(/\/+$/, '');
export const USING_MOCK = !API_URL;

// Bookings must start and end on 15-minute boundaries.
export const SLOT = 15 * 60;
export const toSlot = (t: number) => Math.floor(t / SLOT) * SLOT;

export class ApiError extends Error {
  status: number;
  constructor(message: string, status: number) {
    super(message);
    this.status = status;
  }
}

const MIN = 60;
const now = () => Math.floor(Date.now() / 1000);
const clock = (sec: number) => new Date(sec * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });

export async function currentUserEmail(): Promise<string> {
  const user = await getCurrentUser();
  return user.signInDetails?.loginId ?? user.username;
}

// Members of the Cognito "admin" group see the admin page. The API checks the same group,
// so this only decides whether to show the link.
export async function isAdmin(): Promise<boolean> {
  if (USING_MOCK) return true;
  const groups = (await fetchAuthSession()).tokens?.idToken?.payload['cognito:groups'];
  return Array.isArray(groups) && groups.includes('admin');
}

/* ---------- real backend (used when VITE_API_URL is set) ---------- */

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const session = await fetchAuthSession();
  const token = session.tokens?.idToken?.toString(); // the API wants the ID token, not the access token
  const headers: Record<string, string> = {};
  if (token) headers.Authorization = `Bearer ${token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';

  let res: Response;
  try {
    res = await fetch(`${API_URL}${path}`, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    // The browser hides the reason ("Load failed" / "Failed to fetch"). Usually no network, or a
    // route API Gateway doesn't have: its not-found reply carries no CORS headers.
    throw new ApiError(`Couldn't reach the server for ${method} ${path.split('?')[0]}. Check your connection, or that this route exists in API Gateway.`, 0);
  }
  const text = await res.text();
  if (!res.ok) {
    if (res.status === 401) throw new ApiError('Your sign-in expired. Sign out and sign back in.', 401);
    throw new ApiError(text || `Request failed (${res.status})`, res.status);
  }
  return (text ? JSON.parse(text) : undefined) as T;
}

const real = {
  getRooms: async () => (await call<{ rooms: Room[] }>('GET', '/rooms')).rooms,
  getRoom: (id: string) => call<Room>('GET', `/rooms/${id}`),
  claim: (id: string) => call<Room>('POST', `/rooms/${id}/claim`),
  checkIn: (id: string) => call<Room>('POST', `/rooms/${id}/checkin`),
  end: (id: string) => call<Room>('POST', `/rooms/${id}/end`),
  myReservations: async () =>
    (await call<{ reservations: Reservation[] }>('GET', '/reservations?mine=true')).reservations,
  reserve: (room_id: string, start: number, end: number) =>
    call<Reservation>('POST', '/reservations', { room_id, start, end }),
  cancel: async (res_id: string) => {
    await call<unknown>('DELETE', `/reservations/${res_id}`);
  },
  admin: () => call<AdminView>('GET', '/admin'),
  adminEnd: (id: string) => call<AdminView>('POST', `/admin/rooms/${id}/end`),
  adminSpeed: (seconds_per_minute: number) =>
    call<AdminView>('POST', '/admin/settings', { seconds_per_minute }),
  adminDelete: (room_id: string, sk: string) => call<AdminView>('POST', '/admin/delete', { room_id, sk }),
  adminSlot: (slot_minutes: number) => call<AdminView>('POST', '/admin/settings', { slot_minutes }),
  agent: async (message: string, history: ChatMessage[]) => {
    try {
      return await call<{ reply: string }>('POST', '/agent', { message, history });
    } catch (e) {
      if (e instanceof ApiError && e.status === 404) {
        return { reply: "The assistant isn't connected yet. You can still find and book rooms from the other tabs." };
      }
      throw e;
    }
  },
};

/* ---------- fake backend (in memory, resets on refresh) ---------- */

type MockRoom = Room & { holderEmail?: string };
type MockReservation = Reservation & { user: string; checked_in: boolean };

const rooms: MockRoom[] = ['1', '2', '3'].map((room_id): MockRoom => ({ room_id, state: 'free' }));
const reservations: MockReservation[] = [];

function update(room: MockRoom, me: string): Room {
  const t = now();
  if (room.expires_at && room.expires_at <= t) {
    room.holderEmail = undefined;
    room.expires_at = null;
    room.kind = undefined;
  }
  const next = reservations
    .filter((r) => r.room_id === room.room_id && r.end > t && !(r.checked_in && r.user === room.holderEmail))
    .sort((a, b) => a.start - b.start)[0];
  room.next_res_start = next?.start ?? null;
  if (room.holderEmail) {
    room.state = room.expires_at && room.expires_at - t <= 5 * MIN ? 'warn' : 'held';
  } else {
    room.state = next && next.start - t <= 10 * MIN ? 'reserved' : 'free';
  }
  const { holderEmail, ...shown } = room;
  return holderEmail
    ? { ...shown, holder: holderEmail.split('@')[0], yours: holderEmail === me, presence: 0 }
    : { ...shown, holder: undefined, yours: undefined, kind: undefined, presence: 0 };
}

function findRoom(id: string): MockRoom {
  const room = rooms.find((r) => r.room_id === id);
  if (!room) throw new ApiError(`Room ${id} does not exist.`, 404);
  return room;
}

const asReservation = ({ user: _user, checked_in, ...r }: MockReservation): Reservation => ({
  ...r,
  status: checked_in ? 'checked_in' : 'booked',
});

async function mockMine(): Promise<Reservation[]> {
  const me = await currentUserEmail();
  return reservations
    .filter((r) => r.user === me && r.end > now())
    .sort((a, b) => a.start - b.start)
    .map(asReservation);
}

async function mockReserve(room_id: string, start: number, end: number): Promise<Reservation> {
  findRoom(room_id);
  if (start % SLOT || end % SLOT) throw new ApiError('Start and end must be on 15-minute boundaries', 400);
  if (end <= start) throw new ApiError('End must be after start.', 400);
  if (start < toSlot(now())) throw new ApiError('That time is in the past.', 400);
  if (reservations.some((r) => r.room_id === room_id && r.start < end && start < r.end)) {
    throw new ApiError('That time overlaps another booking.', 409);
  }
  const res: MockReservation = {
    res_id: crypto.randomUUID(),
    room_id,
    start,
    end,
    user: await currentUserEmail(),
    checked_in: false,
  };
  reservations.push(res);
  return asReservation(res);
}

// A tiny stand-in for the Bedrock agent so the chat works without a backend.
async function mockAgent(message: string, _history: ChatMessage[]): Promise<{ reply: string }> {
  const text = message.toLowerCase();
  const me = await currentUserEmail();
  const list = rooms.map((r) => update(r, me));
  const roomMatch = text.match(/room\s*(\d+)/);

  if (text.includes('book') && roomMatch) {
    const room = list.find((r) => r.room_id === roomMatch[1]);
    if (!room) return { reply: `I couldn't find room ${roomMatch[1]}.` };
    if (room.state !== 'free') return { reply: `Room ${room.room_id} isn't free right now.` };
    const start = toSlot(now()) + SLOT;
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
    if (mine.length === 0) return { reply: "You don't have any upcoming bookings." };
    return { reply: mine.map((r) => `Room ${r.room_id}: ${clock(r.start)} to ${clock(r.end)}`).join('\n') };
  }
  if (text.includes('free') || text.includes('available') || text.includes('open')) {
    const free = list.filter((r) => r.state === 'free');
    if (free.length === 0) return { reply: 'Nothing is free right now.' };
    return { reply: `Free right now: ${free.map((r) => `Room ${r.room_id}`).join(', ')}.` };
  }
  return {
    reply: 'In demo mode I only know a few things: ask what\'s free, say "book room 2", or ask for your bookings.',
  };
}

// shortcut: the demo speed dial is only displayed; the fake backend's own timers don't use it.
let mockSpeed = 60;
let mockSlotMinutes = 15;

async function mockAdmin(): Promise<AdminView> {
  const r = (minutes: number) => Math.round(minutes * mockSpeed);
  return {
    seconds_per_minute: mockSpeed,
    slot_minutes: mockSlotMinutes,
    rules: { extension_step: r(5), empty_room_timeout: r(10), reserved_lead: r(10), no_show: r(10), warning: r(5), early_checkin: r(5) },
    rooms: rooms.map((room) => ({
      room_id: room.room_id,
      presence: false,
      session:
        room.holderEmail && room.expires_at
          ? { res_id: `s_${room.room_id}`, user: room.holderEmail, kind: room.kind ?? 'soft', started_at: now(), expires_at: room.expires_at }
          : null,
      reservations: reservations
        .filter((r) => r.room_id === room.room_id)
        .sort((a, b) => b.start - a.start)
        .map((r) => ({ sk: `RES#${r.res_id}`, user: r.user, start: r.start, end: r.end, status: asReservation(r).status })),
      stray_slots: [],
    })),
  };
}

const mock = {
  admin: mockAdmin,
  adminEnd: async (id: string) => {
    const room = findRoom(id);
    if (!room.holderEmail) throw new ApiError(`Room ${id} has no session`, 409);
    room.holderEmail = undefined;
    room.expires_at = null;
    room.kind = undefined;
    return mockAdmin();
  },
  adminDelete: async (_room_id: string, sk: string) => {
    const i = reservations.findIndex((r) => `RES#${r.res_id}` === sk);
    if (i < 0) throw new ApiError(`No row ${sk}`, 404);
    reservations.splice(i, 1);
    return mockAdmin();
  },
  adminSlot: async (slot_minutes: number) => {
    mockSlotMinutes = slot_minutes;
    return mockAdmin();
  },
  adminSpeed: async (seconds_per_minute: number) => {
    if (!(seconds_per_minute >= 1 && seconds_per_minute <= 60)) {
      throw new ApiError('Seconds_per_minute must be between 1 and 60', 400);
    }
    mockSpeed = seconds_per_minute;
    return mockAdmin();
  },
  getRooms: async () => {
    const me = await currentUserEmail();
    return rooms.map((r) => update(r, me));
  },
  getRoom: async (id: string) => update(findRoom(id), await currentUserEmail()),
  claim: async (id: string) => {
    const me = await currentUserEmail();
    const room = findRoom(id);
    if (update(room, me).state !== 'free') throw new ApiError('Room is already held', 409);
    room.holderEmail = me;
    room.kind = 'soft';
    room.expires_at = Math.min(room.next_res_start ?? Infinity, now() + 30 * MIN);
    return update(room, me);
  },
  checkIn: async (id: string) => {
    const me = await currentUserEmail();
    const room = findRoom(id);
    const t = now();
    const res = reservations.find((r) => r.room_id === id && r.user === me && r.start - 5 * MIN <= t && t < r.end);
    if (!res) throw new ApiError('You have no booking for this room right now.', 403);
    res.checked_in = true;
    room.holderEmail = me;
    room.kind = 'hard';
    room.expires_at = res.end;
    return update(room, me);
  },
  end: async (id: string) => {
    const me = await currentUserEmail();
    const room = findRoom(id);
    if (room.holderEmail !== me) throw new ApiError('This is not your session.', 403);
    room.holderEmail = undefined;
    room.expires_at = null;
    room.kind = undefined;
    return update(room, me);
  },
  myReservations: mockMine,
  reserve: mockReserve,
  cancel: async (res_id: string) => {
    const i = reservations.findIndex((r) => r.res_id === res_id);
    if (i >= 0 && reservations[i].checked_in) throw new ApiError('Checked-in bookings can\'t be cancelled. End the session instead.', 409);
    if (i >= 0) reservations.splice(i, 1);
  },
  agent: mockAgent,
};

export const api: typeof real = USING_MOCK ? mock : real;