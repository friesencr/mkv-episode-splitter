"""Run the packaged CLI in isolation and check lossless splits on its target OS."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def video_packets(path):
    result = subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets",
        "-show_data_hash", "sha256", "-show_entries", "packet=data_hash", "-of", "json", path,
    ])
    return [packet["data_hash"] for packet in json.loads(result)["packets"]]


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    binary.chmod(0o755)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        source = work / "Épisode 日本 01.mkv"
        output = work / "Série 日本"
        subprocess.run([
            "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=160x90:r=24:d=6",
            "-c:v", "libx264", "-g", "24", "-bf", "0", "-sc_threshold", "0", source,
        ], check=True)
        expected = video_packets(source)
        parts = [output / f"split_{source.stem}_part{i}.mkv" for i in (1, 2)]
        output.mkdir()
        parts[0].write_bytes(b"previous output")
        env = os.environ.copy()
        env["PATH"] = str(Path(env["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin"
        subprocess.run([binary, "-i", source, "-m", "duration", "-d", "3", "-o", output],
                       check=True, cwd=work, env=env)
        if set(output.iterdir()) != set(parts):
            raise ValueError("Unexpected split filenames")
        actual = []
        for part in parts:
            subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", part, "-f", "null", "-"], check=True)
            packets = video_packets(part)
            if len(packets) != 72:
                raise ValueError(f"{part.name} has {len(packets)} packets, expected 72")
            actual.extend(packets)
        if actual != expected:
            raise ValueError("Split packet hashes differ from the original video")


if __name__ == "__main__":
    main()
