"""modarch command-line interface."""

from __future__ import annotations

import argparse
import os
import sys


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="modarch")
    sub = p.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("map", help="entropy/structure map of an image")
    m.add_argument("image")
    m.add_argument("--block", type=int, default=2048)

    pr = sub.add_parser("prospect", help="sweep PCM interpretations, emit WAVs")
    pr.add_argument("image")
    pr.add_argument("-o", "--outdir", default="prospect_out")
    pr.add_argument("-k", "--top", type=int, default=5)
    pr.add_argument("--rate", type=int, default=48000)

    sy = sub.add_parser("synth", help="write a synthetic test image")
    sy.add_argument("output")
    sy.add_argument("--seconds", type=float, default=4.0)

    sd = sub.add_parser("srd", help="scan for Dolby printmaster (.SRD/.SVA) frame runs")
    sd.add_argument("image")
    sd.add_argument(
        "--extract",
        metavar="PREFIX",
        help="write each run's audio payload to PREFIX_runNN.bin (raw AC-3/AC-2 stream)",
    )

    args = p.parse_args(argv)

    if args.cmd == "synth":
        from modarch.synth import make_synthetic_image

        image, meta = make_synthetic_image(seconds=args.seconds)
        with open(args.output, "wb") as f:
            f.write(image)
        print(
            f"wrote {args.output}: {len(image)} bytes; planted "
            f"{meta['bit_depth']}-bit BE stereo at {meta['payload_offset']:#x}"
        )
        return 0

    with open(args.image, "rb") as f:
        data = f.read()

    if args.cmd == "map":
        from modarch.entropy import entropy_map, summarize

        print(summarize(entropy_map(data, block=args.block)))
        return 0

    if args.cmd == "srd":
        from modarch.srd import extract_payload, find_frame_runs

        runs = find_frame_runs(data)
        if not runs:
            print("no printmaster frame runs found")
            return 1
        for i, r in enumerate(runs):
            print(r.describe())
            if args.extract:
                out = f"{args.extract}_run{i:02d}.bin"
                payload = extract_payload(data, r)
                with open(out, "wb") as f:
                    f.write(payload)
                print(f"  -> {out} ({len(payload)} bytes)")
        return 0

    if args.cmd == "prospect":
        from modarch.prospect import export_wav, prospect

        cands = prospect(data)
        if not cands:
            print("no plausible audio regions found", file=sys.stderr)
            return 1
        os.makedirs(args.outdir, exist_ok=True)
        for i, c in enumerate(cands[: args.top]):
            wav = os.path.join(args.outdir, f"cand{i:02d}.wav")
            export_wav(c, wav, sample_rate=args.rate)
            print(f"{c.describe()}  -> {wav}")
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
