#!/usr/bin/env python3
"""The Ogg to .wem repacker's test: tests/run_wem_test.py WEMTEST [--codebooks FILE] [--oggs DIR]

Ogg Vorbis files are made with sox (libvorbis) from a made-up signal, repacked by wemtest, decoded with vgmstream-cli
and compared with sox's own decode of the Ogg: the same length and within 2 of 32768 a sample. Files the repacker
must refuse are checked for the reason, and damaged copies must not crash it. With --oggs, every .ogg in that folder
goes the same way. Needs sox, vgmstream-cli and the game's codebook library (default: the one rearmed-mods' stems
helper writes); without the library the test is skipped."""
import argparse, array, os, shutil, subprocess, sys, tempfile, wave

checks = 0


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


def samples(path):
    w = wave.open(path)
    return w.getnframes(), array.array("h", w.readframes(w.getnframes()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wemtest")
    ap.add_argument("--codebooks", default=os.path.expanduser("~/Library/Caches/RS Re-ARMed/stems/codebooks.bin"))
    ap.add_argument("--oggs")
    a = ap.parse_args()
    if not os.path.exists(a.codebooks):
        print("skipped: no codebook library at", a.codebooks)
        return
    for tool in ("sox", "vgmstream-cli"):
        if not shutil.which(tool):
            sys.exit("needs " + tool)

    def convert(ogg, out):
        r = subprocess.run([a.wemtest, a.codebooks, ogg, out], capture_output=True, text=True)
        ok(r.returncode in (0, 3), "%s: wemtest: %s%s" % (ogg, r.stdout, r.stderr))
        return r.returncode == 0, r.stdout.strip()

    def same_audio(ogg, tmp, tag, decode=True):
        wem = os.path.join(tmp, tag + ".wem")
        made, said = convert(ogg, wem)
        ok(made, "%s: made (%s)" % (tag, said))
        if not decode:
            return
        subprocess.run(["vgmstream-cli", "-o", wem + ".wav", wem], check=True, capture_output=True)
        subprocess.run(["sox", ogg, "-b", "16", wem + ".ref.wav"], check=True, capture_output=True)
        (n1, x), (n2, y) = samples(wem + ".wav"), samples(wem + ".ref.wav")
        ok(n1 == n2, "%s: %d frames, the Ogg has %d" % (tag, n1, n2))
        worst = max(abs(p - q) for p, q in zip(x, y))
        ok(worst <= 2, "%s: a sample %d from the Ogg's" % (tag, worst))
        ok(max(map(abs, y)) > 1000, tag + ": the signal is not silence")

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "in.wav")
        # 20 s: a sweep, noise bursts and silence, so both block sizes and quiet packets occur
        subprocess.run(["sox", "-n", "-r", "48000", "-c", "2", "-b", "16", wav, "synth", "20", "sine", "80-9000", "pinknoise",
                        "tremolo", "3", "90", "fade", "0.5", "20", "2", "pad", "0.3", "0.7"], check=True, capture_output=True)
        made = [("q5-48k", ["-C", "5"], True), ("q2-48k", ["-C", "2"], True), ("q8-48k", ["-C", "8"], True),
                ("q5-44k", ["-r", "44100", "-C", "5"], True), ("q5-mono", ["-c", "1", "-C", "5"], True),
                # one block size only: vgmstream does not play these, so only that the file is made
                ("q0-22k", ["-r", "22050", "-C", "0"], False)]
        for tag, opts, decode in made:
            ogg = os.path.join(tmp, tag + ".ogg")
            subprocess.run(["sox", wav] + opts + [ogg], check=True, capture_output=True)
            same_audio(ogg, tmp, tag, decode)
        # refusals
        ogg6 = os.path.join(tmp, "six.ogg")
        subprocess.run(["sox", wav, "-c", "6", "-C", "5", ogg6], check=True, capture_output=True)
        open(os.path.join(tmp, "noise.ogg"), "wb").write(os.urandom(5000))
        open(os.path.join(tmp, "empty.ogg"), "wb").close()
        head = open(os.path.join(tmp, "q5-48k.ogg"), "rb").read(3000)
        open(os.path.join(tmp, "headers-only.ogg"), "wb").write(head)
        for name, why in (("six.ogg", "channels"), ("noise.ogg", "not an Ogg file"), ("empty.ogg", "not an Ogg file"),
                          ("headers-only.ogg", "")):
            made_, said = convert(os.path.join(tmp, name), os.path.join(tmp, "no.wem"))
            ok(not made_ and why in said, "%s is refused for its reason: %s" % (name, said))
        if shutil.which("ffmpeg"):  # ffmpeg's own Vorbis encoder: codebooks the game lacks
            nat = os.path.join(tmp, "native.ogg")
            r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", wav, "-c:a", "vorbis", "-strict", "experimental", nat], capture_output=True)
            if r.returncode == 0:
                made_, said = convert(nat, os.path.join(tmp, "no.wem"))
                ok(not made_ and "not one the game has" in said, "another encoder's codebooks are refused: " + said)
        r = subprocess.run([a.wemtest, a.codebooks, os.path.join(tmp, "q5-48k.ogg"), "--fuzz", "1500"], capture_output=True, text=True)
        ok(r.returncode == 0, "damaged copies: %s%s" % (r.stdout, r.stderr[-2000:]))
        print("damaged copies:", r.stdout.strip())
        if a.oggs:
            n = 0
            for name in sorted(os.listdir(a.oggs)):
                if name.endswith(".ogg"):
                    same_audio(os.path.join(a.oggs, name), tmp, "o%d" % n)
                    n += 1
            print("%d Ogg files from %s" % (n, a.oggs))
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
