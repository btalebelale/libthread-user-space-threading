import subprocess
import matplotlib.pyplot as plt
import os

graphs_dir = "graphs"
os.makedirs(graphs_dir, exist_ok=True)

tests = []

for f in os.listdir("tests"):
    path = os.path.join("tests", f)
    if os.path.isfile(path) and os.access(path, os.X_OK):
        if not f.endswith("-pthread"):
            if os.path.exists(os.path.join("tests", f + "-pthread")):
                tests.append(f)

tests.sort()

test_args = {
    "21-create-many": [1, 5, 10, 50, 100, 500, 1000],
    "22-create-many-recursive": [1, 5, 10, 50, 100, 200],
    "23-create-many-once": [1, 5, 10, 50, 100, 500, 1000],
    "31-switch-many": [1, 5, 10, 20, 50],
    "32-switch-many-join": [1, 5, 10, 20, 50],
    "33-switch-many-cascade": [1, 5, 10, 20],
    "51-fibonacci": [5, 10, 15, 20, 25],
    "61-mutex": [1, 5, 10, 50, 100, 500],
    "62-mutex": [1, 5, 10, 50, 100],
}

def run_and_parse(cmd, env=None):
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    for line in result.stdout.splitlines():
        if line.startswith("GRAPH;"):
            parts = line.split(";")
            if parts[-1] == "SUCCESS":
                return float(parts[-2])
            return int(parts[-1]) / 1e6  # us -> s
    return None

for test in tests:
    counts = test_args.get(test)
    if not counts:
        continue

    valid_counts = []
    my_times = []
    pthread_times = []

    for n in counts:
        if test.startswith("31-") or test.startswith("32-"):
            args = [str(n), "20"]
        elif test.startswith("33-"):
            args = [str(n), "10"]
        else:
            args = [str(n)]

        t  = run_and_parse([f"./tests/{test}"] + args,
                           env={"LD_LIBRARY_PATH": "."})
        tp = run_and_parse([f"./tests/{test}-pthread"] + args)

        if t is not None and tp is not None:
            valid_counts.append(n)
            my_times.append(t)
            pthread_times.append(tp)

    if not valid_counts:
        continue

    plt.figure()
    plt.plot(valid_counts, my_times, marker='o', label='libthread')
    plt.plot(valid_counts, pthread_times, marker='s', label='pthread')
    plt.xlabel("Argument")
    plt.ylabel("Temps (s)")
    plt.title(f"Performance {test}")
    plt.legend()
    plt.grid(True)
    plt.savefig(os.path.join(graphs_dir, f"{test}.png"))
    plt.close()
