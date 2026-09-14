#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <semaphore>
#include "tasks/HttpTask.h"
#include "transport/Listener.h"
#include "core/ResultType.h"
#include "tasks/TaskBase.h"

int main() {
    constexpr std::ptrdiff_t RUNNING = 5;
    std::counting_semaphore<RUNNING> sem(RUNNING);

    uint16_t port = 8080;
    tcp::Result<Listener> made = Listener::create(port);
    if (!made) {
        fprintf(stderr, "create failed: %s\n", strerror(made.error()));
        return 1;
    }

    Listener listener = std::move(made.value());
    printf("listening on %d\n", port);

    for (;;) {
        printf("waiting for a client...\n");

        tcp::Result<Connection> incoming = listener.accept();
        if (!incoming) {
            fprintf(stderr, "accept failed: %s\n", strerror(incoming.error()));
            break;
        }

        Connection conn = std::move(incoming.value());
        printf("client connected\n");

        std::unique_ptr<TaskBase> task = std::make_unique<HttpTask>(std::move(conn));

        sem.acquire();

        std::thread worker([t = std::move(task), &sem]() { t->run_task(); sem.release(); });
        worker.detach();
    }
    //Drain
    for (int i = 0; i < RUNNING; i++) {
        sem.acquire();
    }

    printf("\ndone -- listener closes as main returns\n");

    return 0;
}
