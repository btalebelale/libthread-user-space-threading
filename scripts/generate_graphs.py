import subprocess
import time
import matplotlib.pyplot as plt
import os

# créer le dossier "graphs" s'il n'existe pas
graphs_dir = "graphs"
os.makedirs(graphs_dir, exist_ok=True)

tests = ["51-fibonacci"]

#for f in os.listdir("tests"):
 #   path = os.path.join("tests", f)

 #   if os.path.isfile(path) and os.access(path, os.X_OK):
  #      if not f.endswith("-pthread"):
   #         if os.path.exists(os.path.join("tests", f + "-pthread")):
    #            tests.append(f)
#tests.sort()

#thread_counts = [1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100,105, 110, 115, 120]
thread_counts = [4,5,6,7,8,9,10,12,14,16,18,20]


for test in tests:
    #if test == "51-fibonacci":
    #    continue
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

    plt.xlabel("Paramètre de fibonacci")
    plt.ylabel("Temps (s)")
    plt.title(f"Performance {test}")
    plt.legend()
    plt.grid(True)

    # sauvegarde dans le dossier "graphs"
    plt.savefig(os.path.join(graphs_dir, f"{test}.png"))
    plt.show()