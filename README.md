# Room Lock

Smart study-room access: the booking calendar, who is in the room, and whether the door opens, all in one system.

Built by the Muon team for the 2026 BuilderWars Hackathon.

## The problem

Study rooms are hard to get and often wasted. People book a room and never show up, leave early without freeing it, or camp in a room someone else reserved. Paper sign-up sheets and honor-system calendars can't tell who is actually inside, and nothing stops a walk-in from taking a booked room.

> Room Lock ties three things together that are usually separate: the booking calendar, who is physically in the room, and whether the door opens.

## How it works

Every door is **locked by default**. It unlocks only while someone holds the room, and there are two ways to get it.

### Walk-up (soft lock)

Scan the QR code on the door, sign in, and tap **Claim**. If the room is free, nobody is inside, and no booking starts in the next 10 minutes, the door unlocks.

### Reservation (hard lock)

Book a time on the website (in 15-minute steps by default; admins can change the step from 1 to 60 minutes) or ask the AI assistant. When you arrive, scan the QR code and tap **Check in**, or tell the assistant *"I'm here"*, from 5 minutes before your start time. Only the person who booked can check in.

### While you're inside

A session starts with a 5-minute hold and extends itself in 5-minute steps as long as the presence sensor sees someone. Because the room is held only up to a short horizon, the calendar stays bookable and the next booking is never overrun.

### When the session ends, the door locks again

- You tap "I'm done" on the room's page, tap **Check out** on its checkout page (`/r/<room>/checkout`), or press End on the wall device.
- The sensor sees nobody for 10 minutes.
- The next booking is about to start. The device warns 5 minutes ahead with an amber state and a beep.

### No-shows are released automatically

A booking nobody checks in to within 10 minutes of its start is cancelled, and the room becomes available again.

### AI assistant

Users can ask things like *"Is anything free tomorrow at 3?"*, *"Book room 2 for an hour"*, *"Cancel my booking"* or *"I'm here"* (which checks them in and unlocks the door). The assistant answers and acts through the same API as the website, using the user's own login, so it can never do more than the user could by hand. It turns down bookings that overlap someone else's, and anything that isn't about the rooms.

### Admin tools

Members of the admin group get an **Admin** page: they can end any session, delete any booking or leftover time slot, speed up every timing rule for testing (for example, 10 minutes lasting 30 seconds), and change the booking step.

## Tech stack

### AWS services

| Service | Role |
| --- | --- |
| Amazon DynamoDB | Rooms, bookings and taken time slots |
| AWS Lambda &times; 2 | Python (3.12 or newer): one for room interactions, one for the AI agent |
| Amazon Bedrock: Nova | The AI agent's language model |
| Amazon API Gateway | One HTTPS API for the site and the devices |
| Amazon Cognito | User sign-up, sign-in and login tokens |
| AWS Amplify Hosting | Hosts the web app |

### Web app

| Part | Role |
| --- | --- |
| TypeScript static web app | Hosted on AWS Amplify |

### Hardware

| Part | Role |
| --- | --- |
| FreeWili OG | The wall device: display, QR code, buttons, lock control |
| Bottlenose Orca attachment board | Wi-Fi connection to AWS |
| Servo motor | The door lock |
| STHS34PF80 infrared presence sensor | Detects whether someone is in the room |

## Repository layout

| Path | What's there |
| --- | --- |
| [`roomlock-web/`](roomlock-web/) | The web app (React + TypeScript, Vite), deployed by Amplify from `main` |
| [`roomlock-api/`](roomlock-api/) | The room-interaction Lambda: bookings, sessions, device and admin routes, with host tests |
| [`roomlock-agent/`](roomlock-agent/) | The AI agent Lambda behind `POST /agent`, with host tests |
| [`esp32-roomnode/`](esp32-roomnode/) | Firmware for the Wi-Fi board: talks to the API for the wall device |
| [`apps/roomnode/`](apps/roomnode/) | Firmware for the FreeWili OG wall device: display, buttons, lock and sensor |
| [`docs/roomnode.md`](docs/roomnode.md) | How the room node works |
| [`bsp/`](bsp/), [`tools/`](tools/), [`tests/`](tests/) | The FreeWili OG board-support package, its `fw` task runner and host tests. See [`AGENTS.md`](AGENTS.md) |

The two Lambdas are deployed by pasting their `lambda_function.py` into the AWS console, so the copies in this repository are the source of truth: copy any console edit back here.

### Running the tests

```bash
cd roomlock-api && python3 -m unittest test_api
```

```bash
cd roomlock-agent && python3 -m unittest test_agent
```

```bash
cd roomlock-web && npm install && npm run build
```
