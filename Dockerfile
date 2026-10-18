# Build the WebAssembly module for the browser.
FROM emscripten/emsdk:3.1.74 AS wasm-build
WORKDIR /src
COPY main.cpp index.html style.css app.js build_wasm.sh ./
RUN bash build_wasm.sh /build

# Build the optional native HTTP server.
FROM gcc:14-bookworm AS build
WORKDIR /src
COPY main.cpp .
RUN g++ -std=c++17 -O2 -pthread main.cpp -o /langtons_ant_server

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /langtons_ant_server ./langtons_ant_server
COPY --from=wasm-build /build/ ./
ENV PORT=10000
ENV HOST=0.0.0.0
EXPOSE 10000
CMD ["./langtons_ant_server"]
