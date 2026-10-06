"""Mono 24 kHz, low-nibble-first IMA; shared vector with ESP32 protocol v5."""
import struct

STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767)
ADJUST = (-1, -1, -1, -1, 2, 4, 6, 8)


def nibble(state, code):
    step = STEP[state[1]]
    delta = (step >> 3) + (step if code & 4 else 0) + (step >> 1 if code & 2 else 0) + (step >> 2 if code & 1 else 0)
    state[0] = max(-32768, min(32767, state[0] + (-delta if code & 8 else delta)))
    state[1] = max(0, min(88, state[1] + ADJUST[code & 7]))
    return state[0]


def decode(frame):
    if len(frame) < 4 or len(frame) > 483 or frame[2] > 88:
        raise ValueError('Invalid microphone frame')
    state = [struct.unpack('<h', frame[:2])[0], frame[2]]
    samples = [nibble(state, code) for value in frame[3:] for code in (value & 15, value >> 4)]
    return struct.pack('<' + 'h' * len(samples), *samples)


def encode(pcm, state):
    header = struct.pack('<hB', *state)
    codes = []
    for sample in struct.unpack('<' + 'h' * (len(pcm) // 2), pcm):
        delta = sample - state[0]
        code = 8 if delta < 0 else 0
        delta = abs(delta)
        step = STEP[state[1]]
        for bit in (4, 2, 1):
            if delta >= step:
                code |= bit
                delta -= step
            step >>= 1
        nibble(state, code)
        codes.append(code)
    if len(codes) % 2:
        raise ValueError('PCM must contain an even number of samples')
    return header + bytes(codes[i] | codes[i + 1] << 4 for i in range(0, len(codes), 2))
