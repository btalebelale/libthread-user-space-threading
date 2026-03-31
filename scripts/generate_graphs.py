import subprocess
import time
import matplotlib.pyplot as plt
import os

# créer le dossier "graphs" s'il n'existe pas
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

thread_counts = [1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 100, 1000]

for test in tests:
    my_times = []
    pthread_times = []

    print(f"Test en cours : {test}")

    for n in thread_counts:

        # Notre bibliothèque
        start = time.time()
        subprocess.run(
            [f"./tests/{test}", str(n)],
            env={"LD_LIBRARY_PATH": "."},
            stdout=subprocess.DEVNULL
        )
        end = time.time()
        my_times.append(end - start)

        # pthread
        start = time.time()
        subprocess.run(
            [f"./tests/{test}-pthread", str(n)],
            stdout=subprocess.DEVNULL
        )
        end = time.time()
        pthread_times.append(end - start)

    # Graphe
    plt.figure()
    plt.plot(thread_counts, my_times, marker='o', label='libthread')
    plt.plot(thread_counts, pthread_times, marker='s', label='pthread')

    plt.xlabel("Nombre de threads")
    plt.ylabel("Temps (s)")
    plt.title(f"Performance {test}")
    plt.legend()
    plt.grid(True)

    # sauvegarde dans le dossier "graphs"
    plt.savefig(os.path.join(graphs_dir, f"{test}.png"))
    plt.show()