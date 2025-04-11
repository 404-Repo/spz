FROM alpine:3.21 AS builder
RUN echo "http://dl-cdn.alpinelinux.org/alpine/edge/main" >> /etc/apk/repositories && \
    apk update && \
    apk add --no-cache cmake clang lld musl-dev python3 python3-dev py3-pip git ninja
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"
RUN pip install --upgrade pip && \
    pip install scikit-build setuptools wheel
WORKDIR /app
COPY . /app
ENV CC=clang CXX=clang++
RUN pip install .

FROM alpine:3.21
RUN apk add --no-cache libstdc++ python3 && \
    rm -rf /var/cache/apk/*
COPY --from=builder /opt/venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"
RUN adduser -D pyspz
USER pyspz
CMD ["python"]
