from pathlib import Path
p=Path('/root/worktree/zlink-v21-s033/j2l2/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java')
s=p.read_text().replace('        final long nameWord;','        final long nameWord;\n        final long nameTail;')
s=s.replace('            nameWord = name.length() < Long.BYTES ? 0 : ByteBuffer.wrap(prefix).getLong(2);','''            long first = 0, tail = 0;
            for (int index = 0; index < name.length(); index++) {
                if (index < Long.BYTES) first |= (long)name.charAt(index) << (56 - index * 8);
                else tail |= (long)name.charAt(index) << (56 - (index - Long.BYTES) * 8);
            }
            nameWord = first;
            nameTail = tail;''')
a=s.index('    private static boolean ordinaryAsciiWord(');b=s.index('    private static boolean writeAsciiWord(',a)
s=s[:a]+'''    private static long asciiSpecialMask(long word) {
        long high = 0x8080808080808080L;
        long quotes = word ^ 0x2222222222222222L;
        long escapes = word ^ 0x5c5c5c5c5c5c5c5cL;
        long ones = 0x0101010101010101L;
        return (word & high) | ((word - 0x2020202020202020L) & ~word & high)
                | ((quotes - ones) & ~quotes & high) | ((escapes - ones) & ~escapes & high);
    }

    private static boolean ordinaryAsciiWord(long word) {
        return asciiSpecialMask(word) == 0;
    }

'''+s[b:]
s=s.replace('                if (ordinaryAsciiWord(firstWord)) position += Long.BYTES;','''                position += asciiPrefix(firstWord);
                skipAsciiWords();''')
a=s.index('                    long word =',s.index('private int field()'))
b=s.index('                    return 0;',a)
s=s[:a]+'''                    long word = bytes.order() == ByteOrder.BIG_ENDIAN ? firstWord : Long.reverseBytes(firstWord);
                    long mask = length >= Long.BYTES ? -1L : -1L << ((Long.BYTES - length) * 8);
                    for (HeaderField field : HEADER_FIELDS[length]) {
                        if (limit - start >= Long.BYTES) {
                            if ((word & mask) != field.nameWord) continue;
                            if (length <= Long.BYTES) return 1 << field.ordinal();
                            if (limit - start - Long.BYTES >= Long.BYTES) {
                                long tail = bytes.getLong(start + Long.BYTES);
                                if (bytes.order() != ByteOrder.BIG_ENDIAN) tail = Long.reverseBytes(tail);
                                long tailMask = -1L << ((16 - length) * 8);
                                if ((tail & tailMask) == field.nameTail) return 1 << field.ordinal();
                                continue;
                            }
                        }
                        String name = field.name;
                        int offset = limit - start >= Long.BYTES ? Long.BYTES : 0;
                        while (offset < length && bytes.get(start + offset) == name.charAt(offset)) offset++;
                        if (offset == length) return 1 << field.ordinal();
                    }
'''+s[b:]
a=s.index('            while (limit - position >= Long.BYTES',s.index('private String string('))
b=s.index('            while ((position < limit))',a)
s=s[:a]+'''            int asciiStart = position;
            skipAsciiWords();
            characters += position - asciiStart;
'''+s[b:]
loc=s.index('        private int peek() throws IOException')
s=s[:loc]+'''        private int asciiPrefix(long word) {
            long mask = asciiSpecialMask(word);
            if (mask == 0) return Long.BYTES;
            return (bytes.order() == ByteOrder.BIG_ENDIAN ? Long.numberOfLeadingZeros(mask) : Long.numberOfTrailingZeros(mask)) / Byte.SIZE;
        }

        private void skipAsciiWords() {
            while (limit - position >= Long.BYTES) {
                int count = asciiPrefix(bytes.getLong(position));
                position += count;
                if (count < Long.BYTES) return;
            }
        }

'''+s[loc:]
p.write_text(s)
