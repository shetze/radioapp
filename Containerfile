FROM --platform=linux/arm64 quay.io/rhadp-examples/application:latest

# EPEL provides libcaca-devel on AutoSD/CentOS Stream 10. The parent
# application image already supplies the remaining build dependencies.
RUN if [ ! -f /usr/include/caca.h ]; then \
      if command -v dnf >/dev/null 2>&1; then \
        dnf -y install libcaca-devel && dnf clean all; \
      elif command -v microdnf >/dev/null 2>&1; then \
        microdnf install -y libcaca-devel && microdnf clean all; \
      else \
        echo "libcaca-devel is required in the build image" >&2; exit 1; \
      fi; \
    fi

COPY . /opt/src

RUN cmake -S /opt/src -B /opt/build && \
  make -C /opt/build install && \
  rm -rf /opt/src && \
  chown 1001:0 /usr/local/bin/*

RUN ls -la /opt/build
RUN ls -la /usr/local/bin

# missing dirs
RUN mkdir -p /tmp/log && mkdir -p /run/dlt && mkdir -p /run/vsomeip

# permissions & ownership
RUN chown 1001:1001 /tmp/log && chmod -R 777 /tmp/log && \
  chown 1001:0 /run/dlt && chmod -R 777 /run/dlt && \
  chown 1001:0 /run/vsomeip && chmod -R 777 /run/vsomeip

COPY entrypoint.sh /usr/local/bin/

RUN chown 1001:0 /usr/local/bin/*

USER 1001

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]