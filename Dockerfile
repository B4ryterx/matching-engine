# Build the engine with a full toolchain, then ship it on a small Python image.
FROM python:3.12-bookworm AS build
WORKDIR /src
COPY Makefile ./
COPY include include
COPY src src
COPY tests tests
RUN make all CXX=g++ CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -static-libstdc++ -static-libgcc"

FROM python:3.12-slim
WORKDIR /app
COPY --from=build /src/build build
COPY tools tools
COPY ci ci
COPY service service
ARG APP_VERSION=dev
ENV APP_VERSION=$APP_VERSION PORT=8080 VERIFY_ORDERS=50000
EXPOSE 8080
USER nobody
CMD ["python3", "service/app.py"]
