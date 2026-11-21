"""Decode the quiet firmware's version-1 evidence, never infer MCU results."""
import re
import struct

TEXT_MAGIC = 0x4d544c47
BINARY_MAGIC = 0x4d544447


def validate_magic(raw, logging):
    if logging not in ('0', '1'):
        raise ValueError('invalid evidence logging configuration')
    expected = BINARY_MAGIC if logging == '0' else TEXT_MAGIC
    if len(raw) != 4 or int.from_bytes(raw, 'little') != expected:
        raise ValueError('evidence ring magic differs from build configuration')
    return expected


TEMPLATES = {2: 'efuse: result complete=%u status=%u address=%04x\n', 4: 'efuse: config valid=%08x nic=%04x,%04x,%04x country=%u freq=%d\n', 5: 'efuse: phy lna=%d rssi=%d,%d temp=%d bw40=%d channel_valid=%04x rate_valid=%08x\n', 6: 'efuse: nic byte_valid=%02x paths=%u,%u board=%u hw_rf=%u temp_alc=%u ext_lna=%u,%u tx_alc=%u streams=%u,%u antdiv=%u xtal=%u temp_disable=%u coex=%u\n', 7: 'efuse: tssi enabled=%u slope=%u offsets=%u,%u,%u base=%u target=%u\n', 11: 'CAL id=%u status=%u cleanup=%u first=%u second=%u code=%d byte=%u trigger=%u timer=%08x valid=%u dc=%02x%02x%02x%02x%02x%02x\n', 14: 'BW value=%08x status=%u cleanup=%u valid=%u n=%u polls=%u coef=%02x%02x%02x%02x\n', 15: 'DPD temp=%d status=%u cleanup=%u valid=%u n=%u polls=%u gain=%u,%u amp=%u,%u metric=%d,%d disabled=%u,%u timer=%08x\n', 16: 'TSSI value=%u status=%u cleanup=%u valid=%u\n', 17: 'stub: command=%u operation=%u value=%08x\n', 12: 'IQ id=%u mode=%u status=%u cleanup=%u valid=%u measured=%u n=%u polls=%u gi=%u coef=%02x%02x%02x%02x%02x%02x p=%u\n', 13: 'IQ RX raw=%08x,%08x,%08x,%08x,%08x mean=%d,%d rms=%u,%u correction=%d,%d\n', 3: 'efuse: identity chip=%04x ee=%02x fae=%02x mac=%02x%02x%02x%02x%02x%02x\n'}


def signed(word):
    return word - 0x100000000 if word & 0x80000000 else word


def render(kind, words):
    if kind == 18:
        if len(words) != 3:
            raise ValueError('wrong runtime event length')
        event, first, second = words
        if event == 1 and first == second == 0:
            return 'BSP Init done.\n'
        if event in (2, 3) and first and not second:
            return ('Suspend!' if event == 2 else 'ResumeIRQ') + f' count={first}\n'
        if event == 4 and first < 128 and second <= 0x3fff:
            return f'Unknown command:{first}, len:{second}\n'
        if event == 5 and first == 1 and second:
            return f'Firmware ready: COM_REG0={first}, USB reads={second}\n'
        raise ValueError('malformed runtime event')
    if kind == 1:
        if len(words) < 4:
            raise ValueError('short eFuse block record')
        mode, address, status, count = words[:4]
        if mode not in (0, 1) or status > 4 or len(words) != 4 + count or any(x > 255 for x in words[4:]):
            raise ValueError('malformed eFuse block record')
        return f"efuse: {'P' if mode else 'L'} {address:04x} {status} " + bytes(words[4:]).hex() + '\n'
    if kind in (8, 9):
        if len(words) != (14 if kind == 8 else 20):
            raise ValueError('wrong power evidence length')
        return 'efuse: ' + ('channel' if kind == 8 else 'rate') + ' power=' + ','.join(str(signed(x)) for x in words) + '\n'
    if kind == 10:
        if len(words) != 4 or words[0] not in (0, 1):
            raise ValueError('wrong RF/BBP evidence length')
        rf, reg, status, value = words
        return f"RFBBP read {'RF' if rf else 'BBP'} 0:{reg} status={status} value={value:08x}\n"
    if kind not in TEMPLATES:
        raise ValueError('unknown evidence type')
    template = TEMPLATES[kind]
    conversions = re.findall(r'%(?:0?\d*)?([duxX])', template)
    if len(conversions) != len(words):
        raise ValueError('wrong evidence payload length')
    values = tuple(signed(x) if conversion == 'd' else x for conversion, x in zip(conversions, words))
    return template % values


def decode(data):
    offset = sequence = epoch = 0
    lines = []
    while offset < len(data):
        if len(data) - offset < 12:
            raise ValueError('truncated evidence header')
        version, kind, count, actual_sequence, actual_epoch = struct.unpack_from('<BBHII', data, offset)
        size = 12 + 4 * count
        if version != 1 or count > 32 or len(data) - offset < size or actual_sequence != sequence:
            raise ValueError('evidence version, length, overflow, or sequence gap')
        words = struct.unpack_from('<' + 'I' * count, data, offset + 12)
        if kind == 11 and words and words[0] == 1:
            epoch += 1
        if actual_epoch != epoch:
            raise ValueError('evidence initialization epoch differs')
        lines.append(render(kind, words))
        sequence += 1
        offset += size
    return ''.join(lines)


def decode_ring(data, magic):
    if magic == TEXT_MAGIC:
        return data.decode('ascii', errors='replace')
    if magic == BINARY_MAGIC:
        return decode(data)
    raise ValueError('unsupported evidence ring magic')


def decode_capture(data, header):
    """Accept only a complete, coherent ring starting at sequence zero."""
    if isinstance(header, dict):
        magic = int(header['ring_magic'], 0)
        total, lost, capacity = (header[key] for key in
            ('characters_written', 'characters_overwritten', 'capacity'))
    else:
        magic, total, lost, capacity = header
    if capacity != 4096 or lost or total != len(data) or total > capacity:
        raise ValueError('missing or overwritten evidence bytes')
    if magic == BINARY_MAGIC and not data:
        raise ValueError('missing binary evidence')
    return decode_ring(data, magic)


if __name__ == '__main__':
    import argparse
    from pathlib import Path

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='complete binary log from cursor zero')
    args = parser.parse_args()
    print(decode(args.capture.read_bytes()), end='')
