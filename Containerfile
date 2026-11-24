# podman build -f ./Containerfile -t quay.io/shetze/auto-apps:latest .
FROM quay.io/centos/arm64v8:stream10

RUN <<EORUN
cat >/etc/yum.repos.d/autosd-custom.repo <<EOD
[autosd-custom]
name=autosd-custom
baseurl=http://rpmrepo-rpmrepo.apps.ocp.boernig.name/AutoSD-AT/repos/10-stream/
gpgcheck=0
enabled=1
EOD

dnf install epel-release -y
dnf install auto-apps vsomeip3-routingmanager vsomeip3-tools dlt-daemon dlt-tools socat -y

# missing dirs
mkdir -p /tmp/log && mkdir -p /run/dlt && mkdir -p /run/vsomeip

# cleanup
dnf clean all
rm /var/log/* -rf
rm /var/cache/dnf/* -fr
EORUN
