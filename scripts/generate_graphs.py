import subprocess
import time
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
  }

for test in tests:
    counts = test_args.get(test)
    if not counts:
        continue
    my_times = []
    pthread_times = []
    for n in counts:
        if test.startswith("31-") or test.startswith("32-"):
            args = [str(n), "20"]
        elif test.startswith("33-"):
            args = [str(n), "10"]
        else:
            args = [str(n)]
        start = time.time()
        subprocess.run(
            [f"./tests/{test}"] + args,
            env={"LD_LIBRARY_PATH": "."},
            stdout=subprocess.DEVNULL
        )
        end = time.time()
        my_times.append(end - start)
        start = time.time()
        subprocess.run(
            [f"./tests/{test}-pthread"] + args,
            stdout=subprocess.DEVNULL
        )
        end = time.time()
        pthread_times.append(end - start)
    plt.figure()
    plt.plot(counts, my_times, marker='o', label='libthread')
    plt.plot(counts, pthread_times, marker='s', label='pthread')
    plt.xlabel("Argument")
    plt.ylabel("Temps (s)")
    plt.title(f"Performance {test}")
    plt.legend()
    plt.grid(True)
    plt.savefig(os.path.join(graphs_dir, f"{test}.png"))
    plt.show()
