#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <cctype>

// CW (Morse) ID generator for the Fox Hunt beacon — keys a legal station ID
// callsign as an audio-rate tone in complex baseband. Pure stdlib.

namespace foxhunt {

    class CwId {
    public:
        // Returns the Morse pattern for one character ('.'/'-'), empty when
        // the character has no Morse mapping (it is then skipped).
        static const char* morse(char c) {
            switch (::toupper((unsigned char)c)) {
            case 'A': return ".-";    case 'B': return "-...";
            case 'C': return "-.-.";  case 'D': return "-..";
            case 'E': return ".";     case 'F': return "..-.";
            case 'G': return "--.";   case 'H': return "....";
            case 'I': return "..";    case 'J': return ".---";
            case 'K': return "-.-";   case 'L': return ".-..";
            case 'M': return "--";    case 'N': return "-.";
            case 'O': return "---";   case 'P': return ".--.";
            case 'Q': return "--.-";  case 'R': return ".-.";
            case 'S': return "...";   case 'T': return "-";
            case 'U': return "..-";   case 'V': return "...-";
            case 'W': return ".--";   case 'X': return "-..-";
            case 'Y': return "-.--";  case 'Z': return "--..";
            case '0': return "-----"; case '1': return ".----";
            case '2': return "..---"; case '3': return "...--";
            case '4': return "....-"; case '5': return ".....";
            case '6': return "-...."; case '7': return "--...";
            case '8': return "---.."; case '9': return "----.";
            case '/': return "-..-."; case '-': return "-....-";
            default:  return "";
            }
        }

        // Render `text` as interleaved complex float IQ (tone during key-down,
        // silence otherwise) at the given sample rate. wpm uses the PARIS
        // standard (dit = 1.2/wpm seconds). toneHz offsets the carrier so the
        // ID is audible on an SSB/CW receiver tuned to the beacon frequency.
        // amplitude is linear full-scale (0..1).
        static std::vector<float> render(const std::string& text, double sampleRate,
                                         double wpm, double toneHz, float amplitude) {
            std::vector<float> out;
            if (sampleRate <= 0.0 || wpm <= 0.0) { return out; }
            const double ditSec = 1.2 / wpm;
            const int dit = (int)std::lround(ditSec * sampleRate);
            if (dit <= 0) { return out; }

            // Build key envelope in dits: 1=on. Char gap 3 dits, word gap 7.
            auto emit = [&](int units, bool on) {
                int n = units * dit;
                double phaseStep = 2.0 * 3.14159265358979323846 * toneHz / sampleRate;
                static thread_local double phase = 0.0;
                for (int i = 0; i < n; i++) {
                    if (on) {
                        // 5 ms raised-cosine key shaping to avoid clicks.
                        int ramp = (int)(0.005 * sampleRate);
                        float env = 1.0f;
                        if (ramp > 0) {
                            if (i < ramp) { env = 0.5f * (1.0f - cosf(3.1415926f * i / ramp)); }
                            else if (i >= n - ramp) { env = 0.5f * (1.0f - cosf(3.1415926f * (n - 1 - i) / ramp)); }
                        }
                        out.push_back(amplitude * env * (float)cos(phase));
                        out.push_back(amplitude * env * (float)sin(phase));
                    }
                    else {
                        out.push_back(0.0f);
                        out.push_back(0.0f);
                    }
                    phase += phaseStep;
                    if (phase > 6.28318530717958647692) { phase -= 6.28318530717958647692; }
                }
            };

            bool firstChar = true;
            for (size_t ci = 0; ci < text.size(); ci++) {
                char c = text[ci];
                if (c == ' ') { emit(7, false); firstChar = true; continue; }
                const char* pat = morse(c);
                if (!*pat) { continue; }
                if (!firstChar) { emit(3, false); } // inter-character gap
                firstChar = false;
                for (const char* p = pat; *p; p++) {
                    if (p != pat) { emit(1, false); } // intra-character gap
                    emit(*p == '-' ? 3 : 1, true);
                }
            }
            // Trailing word gap so back-to-back IDs don't run together.
            emit(7, false);
            return out;
        }

        // Duration in seconds of the rendered ID (without rendering).
        static double durationSec(const std::string& text, double wpm) {
            if (wpm <= 0.0) { return 0.0; }
            const double dit = 1.2 / wpm;
            double units = 0;
            bool firstChar = true;
            for (char c : text) {
                if (c == ' ') { units += 7; firstChar = true; continue; }
                const char* pat = morse(c);
                if (!*pat) { continue; }
                if (!firstChar) { units += 3; }
                firstChar = false;
                bool firstSym = true;
                for (const char* p = pat; *p; p++) {
                    if (!firstSym) { units += 1; }
                    firstSym = false;
                    units += (*p == '-') ? 3 : 1;
                }
            }
            units += 7;
            return units * dit;
        }
    };
}
