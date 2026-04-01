FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server.c .
RUN gcc -o server server.c -lpthread

EXPOSE 8080
EXPOSE 8081

CMD ["./server", "8080", "8081", "logs.txt"]