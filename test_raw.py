import sys, glob
def test_file(fpath):
    with open(fpath, "rb") as f:
        data = f.read()
    # Find the last 21600 bytes
    if len(data) >= 21600:
        data = data[-21600:]
    count = 0
    for val in data:
        if 2 < val < 150:
            count += 1
    print(f"{fpath}: {count} raw finger pixels")

for f in sorted(glob.glob("/home/championswimmer/Development/Cpp/libfprint-eh577/dumps/*/*.bin")):
    test_file(f)
