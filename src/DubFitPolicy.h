#pragma once
// The numbers a dub has to live inside.
//
// Every constant here was measured on this machine (RX 9070 XT, Tears of Steel
// 734 s / 71 lines) by the Python pipeline in `tools/dubbing`, and none of them
// are estimates. That is the whole reason this file exists: those rules were
// discovered in scripts under F:/tmp/s2st, which is not a repository, and the
// plan document still carries the pre-experiment figures. Implementing against
// the plan's numbers would re-walk every one of these mistakes.
//
// The prose version is docs/DUBBING-FIT-RULES-zh.md; this is the part the code
// can call. Nothing here touches the engine or the dialog yet - it is the
// measuring stick the second pass is built against.

#include "SubtitleTranslatePolicy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace dub_fit {

// ---------------------------------------------------------------------------
// The pace model: how long a line will be before it is synthesized.
// ---------------------------------------------------------------------------

struct PaceModel {
    double perChar{};
    double perComma{};
    double base{};
    double mae{};
};

// Qwen3-TTS-12Hz-1.7B-Base, calibrated on 20 lines: R^2 0.924, MAE 0.236 s.
// The bare rate is 7.02 characters/second, but a comma costs 0.665 s on its own
// - which nothing modelled before, and which is the actual reason long lines
// overflowed. Counting punctuation, the effective rate is 5.45 characters/second.
inline constexpr PaceModel kQwen3TtsPace{0.14246485302172732, 0.6651341975534049,
                                         0.3990104071572027, 0.23599233156837687};

inline double PredictSeconds(const PaceModel& pace, std::size_t characters, std::size_t commas)
{
    return pace.base + pace.perChar * double(characters) + pace.perComma * double(commas);
}

// ---------------------------------------------------------------------------
// The window a line has to fit in.
// ---------------------------------------------------------------------------

// A cap, so that one long pause in the film does not hand a line a fifteen
// second window - at which point fitting it is no longer fitting anything.
inline constexpr double kWindowCapSeconds = 1.5;
// A floor. Below this two neighbours are already touching (see kMinGapSeconds).
inline constexpr double kWindowFloorSeconds = 0.5;

// The most a line may be moved forward into the silence before it. Moving one
// cannot be heard at all, so the only question is how far.
inline constexpr double kMaxShiftForward = 2.0;

// Measured to the next line's *start*, not to this line's own end: a line that
// finishes speaking at 389.71 s still owns the silence up to 390.50 s, and that
// 0.41 s is exactly where a Chinese line needing 1.0 s sits inside an English
// line that took 0.41 s. Using end-start makes almost every line "overflow" and
// sets the whole pipeline racing to compress them.
//
// `nextStart` <= 0 means there is no next line; the cap applies on its own.
inline double WindowSeconds(double start, double end, double nextStart)
{
    const double own = std::max(0.0, end - start);
    double window = own + kWindowCapSeconds;
    if (nextStart > start) window = std::min(window, nextStart - start);
    return std::max(kWindowFloorSeconds, window);
}

// How many seconds this line can possibly be given: its own window plus whatever
// silence it may borrow, stretched by as much as tempo will allow.
//
// Used only to condemn, never to exonerate: the model misses by 0.236 s, so
// "3.0 s will not fit in a 0.50 s window" is a fact while "1.1 s will not fit in
// 1.0 s" is a guess. Spending a synthesis to find out is what this avoids.
inline double ReachableSeconds(double window, double room, double tempoCeiling)
{
    return (window + std::clamp(room, 0.0, kMaxShiftForward)) * tempoCeiling;
}

inline bool IsHopelessBeforeSynthesis(double predicted, double reachable, const PaceModel& pace)
{
    return predicted > reachable + pace.mae;
}

// ---------------------------------------------------------------------------
// The order in which a line gives ground. The order is the product: borrowing
// silence is inaudible, tempo is faintly audible, rewriting the translation
// loses meaning. Reordering these runs fine and sounds wrong.
// ---------------------------------------------------------------------------

enum class Concession {
    Fits,     // it already fits
    Shift,    // start earlier, into the silence the previous line left
    Tempo,    // atempo
    Rewrite,  // ask the translator for a shorter line
    GiveUp,   // report it; mixing will have to cut the tail
};

// The engine pads a one-character "Wow!" with a 0.23 s tail. Trimming is not
// cosmetic, it is budget: left in, fitting spends the whole window compressing
// silence and mixing then cuts real speech off the end.
inline constexpr double kTrimSilenceDb = -48.0;
inline constexpr double kTrimPadSeconds = 0.040;

// ...but it must leave this much, so it does not step on the line before it.
inline constexpr double kShiftClearanceSeconds = 0.15;

// 1.55 and above was judged "rushed" by ear. Short windows hold interjections
// ("No!", "Whoa!"), where the engine's own variance exceeds the window - the pace
// model promises 0.54 s for "No!" and produces 0.96 s - and a person saying an
// interjection really does say it fast. The 1.2 s threshold and the 1.50 ceiling
// are one setting: raising only the ceiling turns ordinary clauses into
// interjections.
inline constexpr double kMaxTempo = 1.25;
inline constexpr double kMaxTempoShort = 1.50;
inline constexpr double kShortWindowSeconds = 1.2;

// Slowing down is allowed too, and for a non-obvious reason: when the Chinese is
// shorter than the line it replaces, the dub stops talking while the picture is
// still speaking and the original voice has already been lifted - leaving music
// under a moving mouth. The target for this is the spoken line itself, not the
// window; using the window would drag a line into the silence after it.
inline constexpr double kMinTempo = 0.85;

// Rewriting loses meaning, measurably: asked to re-say a line in fewer words, the
// translator dropped "I'm freaked out" entirely to save a second. So it is last,
// and it is capped.
inline constexpr int kMaxRewriteRounds = 2;
// Deliberately equal to kMaxTempo: whatever tempo can absorb must never be sent
// to the translator, because asking is the step that damages the sentence.
inline constexpr double kRewriteAbove = kMaxTempo;

// How late a line may start because the one before it is still speaking. A cue
// boundary is a subtitle cut, not a breath, so a dense exchange needs the queue;
// but an unbounded queue drifted this film's dense scene 4 s late - the whole
// exchange then disagrees with the picture, which is worse than the cut it was
// avoiding. Eight tenths is the most a line can slip and still read as synced;
// past it, a shortened line or a cut is the lesser fault.
inline constexpr double kMaxLineDelay = 0.8;

inline double TempoCeilingFor(double window)
{
    return window < kShortWindowSeconds ? kMaxTempoShort : kMaxTempo;
}

// How much earlier this line may start, given the silence available in front of
// it. Never negative; the clearance is not optional.
inline double ShiftFor(double room)
{
    return std::clamp(room - kShiftClearanceSeconds, 0.0, kMaxShiftForward);
}

// Positive means "must be faster", negative "may be slower".
inline double TempoFor(double natural, double available)
{
    if (natural <= 0.0 || available <= 0.0) return 1.0;
    return natural / available;
}

inline double ClampTempo(double tempo, double window, bool allowSlower)
{
    const double floor = allowSlower ? kMinTempo : 1.0;
    return std::clamp(tempo, floor, TempoCeilingFor(window));
}

// Which concession a line needs. `natural` is the duration after trimming; `room`
// is the silence in front of it, already measured against the previous line.
// GiveUp is not decided here. A line that cannot be reached by tempo goes to the
// translator, and only after those rounds are spent without result does it get
// reported - so from here the worst news is "rewrite it".
inline Concession ConcessionFor(double natural, double window, double room)
{
    if (natural <= window) return Concession::Fits;
    const double shifted = window + ShiftFor(room);
    if (natural <= shifted) return Concession::Shift;
    if (natural <= shifted * TempoCeilingFor(window)) return Concession::Tempo;
    return Concession::Rewrite;
}

// ---------------------------------------------------------------------------
// Mixing.
// ---------------------------------------------------------------------------

// Two dubs colliding is the one defect that cannot be hidden: background music
// does not cover it. So mixing gets a harder ceiling than fitting did - here
// there is nothing left to give up instead.
inline constexpr double kHardTempo = 1.35;
inline constexpr double kMinGapSeconds = 0.03;
// A line may run this far past the moment the next one starts. Chinese needs
// about twice the time English does for the same sentence, so a translation
// whose window is one second - "你真是个混蛋，汤姆。" for a 0.7 s English line -
// cannot be said in the time the cue allows no matter what tempo ceiling is
// used, and cutting it is heard as the voice stopping mid-word. Two voices
// overlapping by a fifth of a second is what a real conversation does; a
// sentence that stops before its verb is not.
inline constexpr double kOverhangSeconds = 0.22;

// A line that came out with no Chinese at all must get its own original voice
// back at its own place, otherwise a mouth moves over nothing - the vocal stem
// has been removed whole. It has to be per line: sweeping the vocal track for
// "uncovered speech" treats every line's tail as a miss and poured 60 s of
// English into a twelve minute Chinese track.
inline constexpr bool kRestoreUnvoicedLines = true;

// The bed is at full level everywhere and only ducks where someone is actually
// speaking. A constant duck across the whole track made the film's real silences
// quieter than the source, which listeners reported as "the audio cut out".
// The bed is the separated backing stem - never the source's own mix, which
// carries the English dialogue and would play it a second time.
inline constexpr double kBackingGain = 0.85;      // while a Chinese line plays
inline constexpr double kBackingRest = 1.0;       // everywhere else
inline constexpr double kDuckRampSeconds = 0.150;  // or the duck clicks
inline constexpr double kDuckPadSeconds = 0.20;   // the duck starts before the line, ends after it

// Levels are compared in 20 ms blocks at the 75th percentile, and the mixer and
// the audit must use the same one. Normalizing on whole-file RMS instead pushed
// quiet lines with long silent tails up by 3-4.9 dB.
inline constexpr double kLevelBlockSeconds = 0.020;
inline constexpr double kLevelPercentile = 0.75;
// The engine's own loudness varies by 20 dB (0.017-0.156 RMS); the quietest lines
// are inaudible under music. Clamping the correction keeps one bad line from
// being amplified into noise.
inline constexpr double kGainClampDb = 12.0;

// Master limiting, and why it is done at 4x: a limiter running at 44.1 kHz cannot
// see the peak between two samples, so it does not limit the true peak.
// `level=disabled` or it pulls everything back to 0 dB; `latency=true` measured
// as zero samples of drift against the unprocessed master.
inline constexpr double kPeakLimit = 0.794;  // -2 dBTP
inline constexpr int kLimitOversample = 4;

// A dry mix has to aim at a level, not only at itself. Per-line alignment
// (kGainClampDb) equalizes the generator's own 20 dB spread but leaves the
// whole track wherever the median line happened to land - measured at about
// 7.8 dB under the ceiling, and heard as quieter than the source track beside
// it. So the median line's peak is brought to kDialoguePeak: half the lines sit
// at or just under it, the loudest are caught by the limiter, which is what a
// limiter is for. Scaling to the *loudest* line instead left every normal line
// 6 dB low, because one hot line owned the whole track's headroom.
inline constexpr bool kFillHeadroom = true;
inline constexpr double kDialoguePeak = 0.5;      // -6 dBFS for the median line
inline constexpr double kMaxNormalizeGain = 4.0;  // +12 dB

// ---------------------------------------------------------------------------
// Subtitles, which are grown from the dub rather than translated again.
// ---------------------------------------------------------------------------

// When subtitles were read from the translation draft and the dub from the fitted
// line, 13 of 71 disagreed - the override table and the length rewrites only ever
// touched the latter. Nobody noticed, because that build had no subtitle track at
// all. So: text, start and end all come from the line the engine actually spoke.
inline constexpr double kMinSubtitleHoldSeconds = 0.8;   // but yields to the next speaker
inline constexpr int kMaxSubtitleLineUnits = 32;         // CJK counts as 2
inline constexpr int kCjkUnitsPerCharacter = 2;

// ---------------------------------------------------------------------------
// Facts about the engine these numbers belong to.
// ---------------------------------------------------------------------------

// llama-tts exposes no speed parameter, only --temp, so the XTTS habit of asking
// the synthesizer for a pace does not carry over: every adjustment here is atempo
// applied afterwards.
inline constexpr bool kEngineHasSpeedParameter = false;

// Sampling at --temp 0.7 means the same line twice is not the same length
// (1.12 s and 1.68 s were both observed). The set of overflowing lines therefore
// changes every run: two or three runs, not one, before believing a list of them.
//
// It also means the same line twice is not quite the same voice. Measured on one
// reference clip, one sentence, five draws: 178-233 Hz at 0.7 and 138-170 Hz at
// 0.2 - a quarter of the pitch travel, and a register a fifth lower. That is the
// difference a listener reports as "the timbre changes between lines", so the
// draws are taken at 0.3: nine draws at 0.2 and 0.0 came back with audio every
// time, so this is far from the temperature at which the model wanders into
// silence. Prosody costs a little; a voice that holds still is worth it.
inline constexpr double kDefaultTemperature = 0.3;
// The generation cap, in the model's own 12 Hz frames: 75 s of audio. Without a
// cap a line can run away; with it, the worst case is bounded. The bound is now
// per line - the pace model's own prediction plus headroom, below - because 900
// alone is 75 s: a 13-character line came back as 7.68 s of audio and a 16 s cue
// hit the cap at 72 s, and every second of overshoot is a second atempo must
// compress afterwards, which is audible as a change of timbre.
inline constexpr double kTtsFramesPerSecond = 12.0;
inline constexpr double kTokenHeadroom = 1.6;   // truncation is worse than overshoot
inline constexpr int kMinNewTokens = 48;   // 4 s: a short exclamation, plus air
inline constexpr int kMaxNewTokens = 900;

// The synthesizer emits 24 kHz mono while the working format is 48 kHz stereo, so
// a line has to be resampled and upmixed before it reaches the mix. Resampling
// goes through ffmpeg (soxr, precision 28) rather than a library, and the
// -resampler option belongs on the output side - on the input side ffmpeg exits
// non-zero.
inline constexpr int kEngineSampleRate = 24000;
inline constexpr int kWorkingSampleRate = 48000;
inline constexpr int kWorkingChannels = 2;

// The audio track's language tag, in ISO 639-2/B. This is the same trap the
// subtitle track fell into: writing the terminological code (zho) into a MOV/MP4
// is dropped silently, and the finished track then has no language at all, so a
// player choosing by language cannot see it.
inline std::string TrackLanguageCode(std::string_view tag)
{
    return subtitle_translate::MuxLanguageCode(tag);
}

// ---- Who speaks ------------------------------------------------------------
// step1_align.py hard-codes 0.55 and that number was copied here. Measured on
// this film against the Python pipeline's own 19 hand-labelled lines (robot /
// celia / tom), 0.55 is far too strict for it: same-speaker pairs sit at
// 0.18-0.39 cosine and different-speaker pairs at 0.07-0.16, so 0.55 gives
// every line a voice of its own - 18 "voices" for 18 lines, purity 1.00 only
// because nothing is ever merged. Scored across thresholds, 0.125 is the best
// the embedding supports here: 6 voices, purity 0.89, and every voice with more
// than two lines comes out with one actor in it. Going lower (0.10) starts
// merging Tom into Celia, which is the fault a listener hears as "the man is
// speaking with a woman's voice".
//
// The gap between 0.18 and 0.16 is thin, and the reason is the material, not
// the graph: film dialogue is short, mixed and acted. A second opinion was
// tried and rejected - CAM++ scored same-speaker 0.43 against different-speaker
// 0.67, and sherpa-onnx's full diarization split these 180 s into twenty
// speakers. Pitch was the other candidate and does not separate them either:
// the film's male lead measures 210-285 Hz on his own long lines.
// Two means are split by counting, not by a threshold: the film's lines are
// k-means'd into two, every seed pair tried, and the partition with the least
// inertia wins. A threshold could not do this job here - measured on the film's
// own dialogue, one voice's lines sit at 0.18-0.39 against each other while two
// different voices reach 0.16, so no single number separates them. Comparing a
// line to *both* means and taking the nearer one does separate them: with the
// reference means from the film's own lines, 10 of 10 lines landed on the right
// speaker. What a threshold could not do at all was keep a female line from
// being spoken by the male clip, which is what a listener reported.
//
// So kClusterThreshold is gone. What remains is how far apart two means must be
// before a cluster is worth splitting again: below this the sub-means are one
// actor heard at two distances, and splitting him in two would hand the film
// two timbres for one person.
inline constexpr double kClusterSplitCeiling = 0.22;
// A voice heard once or twice cannot yield 6 s of reference; it borrows the
// nearest voice's clip rather than being spoken by an unclonable stranger.
inline constexpr size_t kClusterMinLines = 2;

// A reference clip: one good sentence of one voice, cut from the vocal stem.
// Short is deliberate. Held everything else still - same seven Chinese texts,
// same --temp 0.3, same -n rule - and moved only the clip:
//
//     3 s, one sentence      49-64 Hz of pitch travel across the seven lines
//     6 s, one sentence     126 Hz
//    12 s, three sentences  132 Hz
//
// The engine does not keep a longer sample better; it wanders inside it, and
// that is what a listener reports as the timbre changing between lines. So the
// target is ~4 s and nothing may push past 6 s, well inside the engine's own
// 10-15 s window. Below 1.2 s a line carries no timbre at all.
inline constexpr double kReferenceMinLine = 1.2;
inline constexpr double kReferenceMaxLine = 5.0;
// Two sentences, not one. Tried a 3 s single sentence first - it cut the spread
// of seven fixed texts from 132 Hz to 49 Hz, and looked like the answer - and
// then the film came back with every line, including the male lead, at
// 255-343 Hz: one sentence is not enough for the engine to hold on to, and it
// substitutes its own speaker. Two sentences (this excerpt: 6 s) put the male
// lead at 117-209 Hz around his own 202 Hz, and the female lead at 272/273 Hz on
// both of her lines.
//
// The spread of pitches across *different* lines is not a useful measure - it is
// mostly the prosody of the words - so this number is settled by where the voice
// lands, not by how tightly it clusters.
inline constexpr double kReferenceTarget = 4.0;
inline constexpr double kReferenceCap = 6.0;
// The length the seed prefers, measured above: a sentence of two to five seconds
// clones the steadiest. Longer sentences are tried only when none of that size
// exists for the voice.
inline constexpr double kReferenceSeedBestLow = 2.0;
inline constexpr double kReferenceSeedBestHigh = 5.0;
// Lowered from 6 s: a short excerpt can hold only one or two clean sentences per
// character, and a 4 s clip of the right person is worth far more than a 15 s
// clip of three people. Measured on the 3-minute excerpt, the female lead had
// exactly one usable sentence (4.1 s); at a 6 s floor she had no clip at all,
// borrowed the male lead's, and the whole part was read by a man.
inline constexpr double kReferenceMinTotal = 3.0;
inline constexpr double kReferenceFade = 0.005;
inline constexpr double kReferenceGap = 0.12;
// How alike two lines must sound to be joined into one reference clip. A cluster
// is not a person - it is whatever the split could not separate - so a clip is
// grown from one good line and then only accepts lines that measure like it.
// Measured on this material, two lines of the same actor reach 0.18-0.51 and two
// different actors 0.00-0.16, so 0.30 admits the actor and refuses the stranger.
inline constexpr double kReferenceAgree = 0.30;
// How alike a line has to be to a finished reference clip to be read as that
// person. Below this the line is somebody the film has not given a voice: an
// announcer counting down to a launch, a robot, a man in another scene. Those
// lines used to be handed to whichever mean was marginally nearer - a
// difference of 0.07 against 0.15, which is noise - and that is how a launch
// countdown ended up spoken by the female lead. An unreckonable line now goes
// to the longest reference, i.e. the actor the film has most of.
inline constexpr double kCastUncertain = 0.12;
// One utterance, not one cue. Whisper cuts a sentence at its pauses and the text
// stage keeps a cue per cut; spoken cue by cue, a sentence arrives as four lines
// in four voices. A cue that stops mid-sentence and is followed within this gap
// by the one that goes on is one line for the voice (the subtitle keeps its own
// cues). The cap keeps a monologue from becoming one 40 s line whose Chinese
// could never fit.
inline constexpr double kUtteranceGap = 0.35;
inline constexpr double kUtteranceMaxSeconds = 16.0;
// A cue may only serve as a reference when the vocal stem really carries it:
// this much more energy in the voice than in the backing at the same moment.
// An actor's line measures far above it; a channel watermark or a music cue that
// whisper turned into words sits at about 1, and cloning one gives five lines to
// an announcer who is not in the film.
//
// It is a preference, not a veto. Measured on the film's dialogue, one cluster
// had every line under the old bar of 2.0 (0.6x, 0.9x, 1.27x) because the scene
// has music under the whole conversation - so it produced no clip at all and
// its lines borrowed the other cluster's voice, which is heard as "the woman is
// spoken by a man". Purity now only orders the candidates; when the strict set
// is too short to make a clip, the filtered-out lines are used as well.
inline constexpr double kReferenceMinPurity = 1.0;

// The pitch the fallback splits its two voices on. Male speech sits around
// 85-180 Hz and female around 165-255, so 165 is the middle of the overlap; a
// cue within a few hertz of it is a coin toss either way, which the neighbour
// rule absorbs. Only used when there is no embedder: it cannot separate two men,
// which is the whole reason the embedding is the primary answer.
inline constexpr double kVoiceSplitHz = 165.0;
inline constexpr double kVoiceMinVoiced = 0.20;

}  // namespace dub_fit
