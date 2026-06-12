import sys, glob
def test_file(fpath):
    with open(fpath, "rb") as f:
        data = f.read()
    if len(data) >= 21600:
        data = data[-21600:]
    count2 = sum(1 for v in data if 2 < v < 150)
    count15 = sum(1 for v in data if 15 < v < 150)
    count30 = sum(1 for v in data if 30 < v < 150)
    print(f"{fpath}: >2: {count2}, >15: {count15}, >30: {count30}")

for f in sorted(glob.glob("/home/championswimmer/Development/Cpp/libfprint-eh577/dumps/libfprint-guided-20260610-000413/*.bin")):
    test_file(f)
for f in sorted(glob.glob("/home/championswimmer/Development/Cpp/libfprint-eh577/dumps/phantom-20260611-121359/*.bin")):
    test_file(f)
