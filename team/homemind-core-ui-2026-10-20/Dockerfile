# syntax=docker/dockerfile:1
# Contracts come from <repo>/contracts/pfn.v1 through a named build context:
#   docker buildx build --build-context contracts=../../contracts/pfn.v1 ...
FROM ubuntu:24.04 AS build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config mosquitto libmosquitto-dev libsqlite3-dev \
    nlohmann-json3-dev python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt /src/CMakeLists.txt
COPY src /src/src
COPY tests /src/tests
COPY package /src/package
COPY --from=contracts . /src/contracts
RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build

RUN mkdir -p /out/bin /out/lib \
    && cp /build/homemind-core /build/homemind-selftest /build/homemind-soaktest \
      /build/homemind-simulator /out/bin/ \
    && cp /usr/sbin/mosquitto /out/bin/mosquitto.real \
    && cp /src/package/mosquitto-wrapper.sh /out/bin/mosquitto \
    && mosquitto_lib="$(find /usr/lib -name libmosquitto.so.1 -print -quit)" \
    && dlt_lib="$(find /usr/lib -name libdlt.so.2 -print -quit)" \
    && websockets_lib="$(find /usr/lib -name libwebsockets.so.19 -print -quit)" \
    && test -n "$mosquitto_lib" \
    && test -n "$dlt_lib" \
    && test -n "$websockets_lib" \
    && cp -L "$mosquitto_lib" "$dlt_lib" "$websockets_lib" /out/lib/ \
    && chmod 0755 /out/bin/*

FROM build AS ipk-build

COPY deploy /package-input/deploy
COPY package /package-input/package
COPY --from=contracts . /package-input/contracts

RUN set -eu; \
    service_dir=/pkg/data/data/share/homemind; \
    unit_dir=/pkg/data/data/share/usr/lib/systemd/system; \
    mkdir -p "$service_dir/bin" "$service_dir/lib" "$service_dir/config" \
      "$service_dir/package-defaults" \
      "$unit_dir/homemind-mqtt.service.d" "$unit_dir/multi-user.target.wants" \
      /pkg/control /out-ipk; \
    cp /build/homemind-core /build/homemind-selftest /build/homemind-soaktest \
      /build/homemind-simulator \
      "$service_dir/bin/"; \
    cp /package-input/contracts/sensor-rules.json \
      /package-input/contracts/scenario-rules.json \
      /package-input/contracts/device-registry.json \
      /package-input/contracts/task-policy.json \
      /package-input/contracts/context-rules.json "$service_dir/config/"; \
    cp /usr/sbin/mosquitto "$service_dir/bin/mosquitto.real"; \
    cp /package-input/package/mosquitto-wrapper.sh "$service_dir/bin/mosquitto"; \
    mosquitto_lib="$(find /usr/lib -name libmosquitto.so.1 -print -quit)"; \
    dlt_lib="$(find /usr/lib -name libdlt.so.2 -print -quit)"; \
    websockets_lib="$(find /usr/lib -name libwebsockets.so.19 -print -quit)"; \
    test -n "$mosquitto_lib"; \
    test -n "$dlt_lib"; \
    test -n "$websockets_lib"; \
    cp -L "$mosquitto_lib" "$dlt_lib" "$websockets_lib" "$service_dir/lib/"; \
    cp /package-input/package/mosquitto-fresh-install.conf \
      "$service_dir/package-defaults/mosquitto.conf"; \
    cp /package-input/package/mosquitto.acl "$service_dir/package-defaults/mosquitto.acl"; \
    cp /package-input/deploy/homemind-core.service \
      /package-input/deploy/homemind-ethernet.service \
      /package-input/package/homemind-mqtt.service \
      "$unit_dir/"; \
    cp /package-input/deploy/homemind-mqtt-ethernet.conf \
      "$unit_dir/homemind-mqtt.service.d/10-ethernet.conf"; \
    ln -s ../homemind-ethernet.service \
      "$unit_dir/multi-user.target.wants/homemind-ethernet.service"; \
    ln -s ../homemind-mqtt.service \
      "$unit_dir/multi-user.target.wants/homemind-mqtt.service"; \
    ln -s ../homemind-core.service \
      "$unit_dir/multi-user.target.wants/homemind-core.service"; \
    cp /package-input/package/control /pkg/control/control; \
    cp /package-input/package/conffiles /pkg/control/conffiles; \
    cp /package-input/package/postinst /pkg/control/postinst; \
    cp /package-input/package/prerm /pkg/control/prerm; \
    chmod 0755 /pkg/control/postinst /pkg/control/prerm \
      "$service_dir/bin/"*; \
    printf '2.0\n' >/pkg/debian-binary; \
    tar --numeric-owner --owner=0 --group=0 -C /pkg/control -czf /pkg/control.tar.gz .; \
    tar --numeric-owner --owner=0 --group=0 -C /pkg/data -czf /pkg/data.tar.gz .; \
    cd /pkg; \
    ar r /out-ipk/homemind-core_0.6.5_aarch64.ipk \
      debian-binary control.tar.gz data.tar.gz

FROM scratch AS artifact
COPY --from=build /out/ /

FROM scratch AS ipk
COPY --from=ipk-build /out-ipk/ /
