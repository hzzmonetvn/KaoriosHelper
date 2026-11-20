/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 *
 * Self-contained RFC 8259 JSON parser for system_server DEX runtime and host JVM tests.
 */

package org.frknkrc44.hma_oss.zygote;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class SimpleJson {
    private final String src;
    private final int len;
    private int pos;

    private SimpleJson(String src) {
        this.src = src;
        this.len = src.length();
        this.pos = 0;
    }

    public static Map<String, Object> parseObject(String json) {
        Object root = parse(json);
        if (!(root instanceof Map)) {
            throw new IllegalArgumentException("JSON root must be an object");
        }
        @SuppressWarnings("unchecked")
        Map<String, Object> map = (Map<String, Object>) root;
        return map;
    }

    public static Object parse(String json) {
        if (json == null) {
            throw new IllegalArgumentException("JSON input is null");
        }
        SimpleJson parser = new SimpleJson(json);
        parser.skipWhitespace();
        if (parser.pos >= parser.len) {
            throw new IllegalArgumentException("JSON input is empty");
        }
        Object value = parser.readValue();
        parser.skipWhitespace();
        if (parser.pos != parser.len) {
            throw new IllegalArgumentException("Unexpected trailing characters at position " + parser.pos);
        }
        return value;
    }

    private void skipWhitespace() {
        while (pos < len) {
            char c = src.charAt(pos);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                pos++;
            } else {
                break;
            }
        }
    }

    private Object readValue() {
        skipWhitespace();
        if (pos >= len) {
            throw new IllegalArgumentException("Unexpected end of JSON input");
        }
        char c = src.charAt(pos);
        switch (c) {
            case '{':
                return readObject();
            case '[':
                return readArray();
            case '"':
                return readString();
            case 't':
            case 'f':
                return readBoolean();
            case 'n':
                return readNull();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return readNumber();
                }
                throw new IllegalArgumentException("Unexpected character '" + c + "' at position " + pos);
        }
    }

    private Map<String, Object> readObject() {
        pos++; // consume '{'
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        skipWhitespace();
        if (pos < len && src.charAt(pos) == '}') {
            pos++;
            return map;
        }
        while (true) {
            skipWhitespace();
            if (pos >= len || src.charAt(pos) != '"') {
                throw new IllegalArgumentException("Expected string key at position " + pos);
            }
            String key = readString();
            skipWhitespace();
            if (pos >= len || src.charAt(pos) != ':') {
                throw new IllegalArgumentException("Expected ':' after key at position " + pos);
            }
            pos++; // consume ':'
            Object value = readValue();
            map.put(key, value);
            skipWhitespace();
            if (pos >= len) {
                throw new IllegalArgumentException("Unterminated object");
            }
            char next = src.charAt(pos);
            if (next == '}') {
                pos++;
                return map;
            }
            if (next != ',') {
                throw new IllegalArgumentException("Expected ',' or '}' at position " + pos);
            }
            pos++; // consume ','
        }
    }

    private List<Object> readArray() {
        pos++; // consume '['
        List<Object> list = new ArrayList<Object>();
        skipWhitespace();
        if (pos < len && src.charAt(pos) == ']') {
            pos++;
            return list;
        }
        while (true) {
            list.add(readValue());
            skipWhitespace();
            if (pos >= len) {
                throw new IllegalArgumentException("Unterminated array");
            }
            char next = src.charAt(pos);
            if (next == ']') {
                pos++;
                return list;
            }
            if (next != ',') {
                throw new IllegalArgumentException("Expected ',' or ']' at position " + pos);
            }
            pos++; // consume ','
        }
    }

    private String readString() {
        pos++; // consume opening '"'
        StringBuilder sb = new StringBuilder();
        while (pos < len) {
            char c = src.charAt(pos++);
            if (c == '"') {
                return sb.toString();
            }
            if (c == '\\') {
                if (pos >= len) {
                    throw new IllegalArgumentException("Unterminated escape sequence");
                }
                char esc = src.charAt(pos++);
                switch (esc) {
                    case '"': sb.append('"'); break;
                    case '\\': sb.append('\\'); break;
                    case '/': sb.append('/'); break;
                    case 'b': sb.append('\b'); break;
                    case 'f': sb.append('\f'); break;
                    case 'n': sb.append('\n'); break;
                    case 'r': sb.append('\r'); break;
                    case 't': sb.append('\t'); break;
                    case 'u': {
                        if (pos + 4 > len) {
                            throw new IllegalArgumentException("Incomplete unicode escape");
                        }
                        String hex = src.substring(pos, pos + 4);
                        pos += 4;
                        sb.append((char) Integer.parseInt(hex, 16));
                        break;
                    }
                    default:
                        throw new IllegalArgumentException("Invalid escape character '\\" + esc + "'");
                }
            } else if (c < 0x20) {
                throw new IllegalArgumentException("Unescaped control character in JSON string");
            } else {
                sb.append(c);
            }
        }
        throw new IllegalArgumentException("Unterminated string literal");
    }

    private Boolean readBoolean() {
        if (src.startsWith("true", pos)) {
            pos += 4;
            return Boolean.TRUE;
        }
        if (src.startsWith("false", pos)) {
            pos += 5;
            return Boolean.FALSE;
        }
        throw new IllegalArgumentException("Invalid boolean literal at position " + pos);
    }

    private Object readNull() {
        if (src.startsWith("null", pos)) {
            pos += 4;
            return null;
        }
        throw new IllegalArgumentException("Invalid null literal at position " + pos);
    }

    private Number readNumber() {
        int start = pos;
        if (src.charAt(pos) == '-') pos++;
        if (pos >= len || src.charAt(pos) < '0' || src.charAt(pos) > '9') {
            throw new IllegalArgumentException("Invalid number at position " + start);
        }
        if (src.charAt(pos) == '0') {
            pos++;
        } else {
            while (pos < len && src.charAt(pos) >= '0' && src.charAt(pos) <= '9') pos++;
        }
        boolean isDecimal = false;
        if (pos < len && src.charAt(pos) == '.') {
            isDecimal = true;
            pos++;
            if (pos >= len || src.charAt(pos) < '0' || src.charAt(pos) > '9') {
                throw new IllegalArgumentException("Invalid fractional number at position " + start);
            }
            while (pos < len && src.charAt(pos) >= '0' && src.charAt(pos) <= '9') pos++;
        }
        if (pos < len && (src.charAt(pos) == 'e' || src.charAt(pos) == 'E')) {
            isDecimal = true;
            pos++;
            if (pos < len && (src.charAt(pos) == '+' || src.charAt(pos) == '-')) pos++;
            if (pos >= len || src.charAt(pos) < '0' || src.charAt(pos) > '9') {
                throw new IllegalArgumentException("Invalid exponent in number at position " + start);
            }
            while (pos < len && src.charAt(pos) >= '0' && src.charAt(pos) <= '9') pos++;
        }
        String token = src.substring(start, pos);
        if (isDecimal) {
            return Double.valueOf(token);
        }
        try {
            return Long.valueOf(token);
        } catch (NumberFormatException e) {
            return Double.valueOf(token);
        }
    }
}
