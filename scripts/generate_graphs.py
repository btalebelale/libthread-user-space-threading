import subprocess
import time
import matplotlib.pyplot as plt

tests = [
    "21-create-many",
    "31-switch-many"
]

thread_counts = [10, 50, 100, 200, 500]

for test in tests:
    my_times = []
    pthread_times = []

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

        #  pthread
        start = time.time()
        subprocess.run(
            [f"./tests/{test}-pthread", str(n)],
            stdout=subprocess.DEVNULL
        )
        end = time.time()
        pthread_times.append(end - start)

    # --- Graphe ---
    plt.figure()
    plt.plot(thread_counts, my_times, marker='o', label='libthread')
    plt.plot(thread_counts, pthread_times, marker='s', label='pthread')

    plt.xlabel("Nombre de threads")
    plt.ylabel("Temps (s)")
    plt.title(f"Performance {test}")
    plt.legend()
    plt.grid(True)

    plt.savefig(f"{test}.png")
    plt.show()