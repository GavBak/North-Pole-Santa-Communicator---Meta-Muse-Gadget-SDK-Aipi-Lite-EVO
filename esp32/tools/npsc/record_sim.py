"""Host replay of the 5-round "hey santa" recording session in muse_voice.c.

Models one round: 300 ms cue-beep discard (not part of the clip), then the
fix1 end-of-speech rule with a 5.0 s start-timeout (a miss / skipped round),
a 3.0 s cap once speech has started, and 200 ms of audio kept before the
first speech. A talk press or menu press at any time cancels the session.
Wake-word hits during the session are ignored (they do not start a turn).

The intro line and "Again!" are spoken cues; they are not part of this
audio state machine.
"""
import io
from pathlib import Path
import numpy as np

R, C = 16000, 320
DISCARD_CHUNKS = 15          # 300 ms after the beep
START_S = 5.0
CAP_S = 3.0
PRE_S = 0.2
QUIET_CHUNKS = 50            # 1.0 s
SPEECH_DB = 10.0
SPEECH_CHUNKS = 3
FLOOR_MIN = -90.0
GRACE = 0                    # follow-up style: no 200 ms "hey santa" grace
ROUNDS = 5


def dbfs(x):
    return max(-100.0, 10 * np.log10(np.mean(x.astype(np.float64) ** 2) / 32768 ** 2 + 1e-30))


def track_floor(floor, db):
    if db < FLOOR_MIN:
        return floor
    return floor + (-0.5 if db < floor else 0.03)


def chunks_of(pcm):
    pcm = np.asarray(pcm, dtype=np.int16)
    n = (len(pcm) // C) * C
    return [pcm[k : k + C] for k in range(0, n, C)]


def tone(seconds, hz=700, amp=8000):
    t = np.arange(int(seconds * R)) / R
    return (amp * np.sin(2 * np.pi * hz * t)).astype(np.int16)


def silence(seconds):
    return np.zeros(int(seconds * R), dtype=np.int16)


def record_round(pcm_after_beep, floor, talk_at=None, menu_at=None):
    """Replay one local hands-free record() after the 300 ms discard.

    Returns dict: t_end, spoke, saved, reason, clip_from, clip_frames, floor.
    Times are seconds from the start of record() (after the discard).
    """
    chunks = chunks_of(pcm_after_beep)
    loud = quiet = 0
    spoke = False
    clip_from = 0
    stop_at_s = None
    for i, ch in enumerate(chunks):
        t = (i + 1) * C / R
        if talk_at is not None and t >= talk_at:
            return dict(t_end=t, spoke=False, saved=False, reason="talk press",
                        clip_from=0, clip_frames=0, floor=floor)
        if menu_at is not None and t >= menu_at:
            return dict(t_end=t, spoke=False, saved=False, reason="menu",
                        clip_from=0, clip_frames=0, floor=floor)
        db = dbfs(ch)
        grace = i + 1 <= GRACE
        loud = loud + 1 if (not grace and db > floor + SPEECH_DB) else 0
        if loud >= SPEECH_CHUNKS:
            if not spoke:
                pre = int(PRE_S * R)
                from_frames = max(0, (i + 1) * C - pre)
                clip_from = from_frames
                stop_at_s = from_frames / R + CAP_S
            spoke = True
            quiet = 0
        else:
            quiet += 1
        floor = track_floor(floor, db)
        if spoke and quiet >= QUIET_CHUNKS:
            n = (i + 1) * C
            return dict(t_end=t, spoke=True, saved=True, reason="1 s quiet",
                        clip_from=clip_from, clip_frames=n - clip_from, floor=floor)
        if not spoke and t >= START_S:
            return dict(t_end=t, spoke=False, saved=False, reason="nothing said",
                        clip_from=0, clip_frames=0, floor=floor)
        if stop_at_s is not None and t >= stop_at_s:
            n = int(stop_at_s * R)
            return dict(t_end=stop_at_s, spoke=True, saved=True, reason="3 s cap",
                        clip_from=clip_from, clip_frames=n - clip_from, floor=floor)
    n = len(chunks) * C
    return dict(t_end=None, spoke=spoke, saved=spoke, reason="ran out",
                clip_from=clip_from, clip_frames=max(0, n - clip_from) if spoke else 0,
                floor=floor)


def session(rounds):
    """rounds: list of (pcm_after_beep, kwargs) or 'cancel' reasons mid-way."""
    saved = 0
    log = []
    for i, item in enumerate(rounds, 1):
        if item is None:
            break
        pcm, kw = item if isinstance(item, tuple) else (item, {})
        floor = kw.pop("floor", -60.0)
        r = record_round(pcm, floor, **kw)
        r["round"] = i
        if r["saved"]:
            saved += 1
        log.append(r)
        if r["reason"] in ("talk press", "menu"):
            break
    return saved, log


def main():
    floor = -60.0
    speech = np.concatenate([silence(0.4), tone(0.8), silence(1.5)])
    long_speech = np.concatenate([silence(0.2), tone(4.0), silence(1.0)])
    quiet = silence(6.0)

    r = record_round(speech, floor)
    assert r["saved"] and r["reason"] == "1 s quiet", r
    assert 0.9 < r["clip_frames"] / R < 2.2, r  # 0.2 pre + 0.8 + 1.0 quiet
    # First speech is flagged after 60 ms of tone at t=0.40, so clip_from is ~0.26 s.
    assert 0.15 < r["clip_from"] / R < 0.35, r

    r = record_round(quiet, floor)
    assert (not r["saved"]) and r["reason"] == "nothing said", r
    assert abs(r["t_end"] - START_S) < 0.05, r

    r = record_round(long_speech, floor)
    assert r["saved"] and r["reason"] == "3 s cap", r
    assert abs(r["clip_frames"] / R - CAP_S) < 0.05, r

    r = record_round(np.concatenate([silence(0.3), tone(0.5), silence(2.0)]), floor, talk_at=0.1)
    assert (not r["saved"]) and r["reason"] == "talk press", r

    r = record_round(speech, floor, menu_at=0.5)
    assert (not r["saved"]) and r["reason"] == "menu", r

    n, log = session([speech, quiet, speech, long_speech, speech])
    assert n == 4 and log[1]["reason"] == "nothing said" and len(log) == 5, (n, log)
    assert all(r["round"] == i for i, r in enumerate(log, 1))

    n, log = session([speech, speech, (speech, dict(talk_at=0.05)), speech, speech])
    assert n == 2 and len(log) == 3 and log[-1]["reason"] == "talk press", (n, log)

    print("record_sim: all checks passed")
    print(f"  rounds={ROUNDS}, start={START_S}s, cap={CAP_S}s, quiet=1.0s, discard=300ms, pre=200ms")
    print(f"  example session saved {4} of {ROUNDS} (one skip, no cancel)")


if __name__ == "__main__":
    main()
