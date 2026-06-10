#!/bin/sh

# Create directory structure
echo Create directory structure
mkdir sdrpp_debian_amd64
mkdir sdrpp_debian_amd64/DEBIAN

# Create package info
echo Create package info
echo Package: predator-rf >> sdrpp_debian_amd64/DEBIAN/control
echo Version: 1.1.0$BUILD_NO >> sdrpp_debian_amd64/DEBIAN/control
echo Maintainer: Predator RF >> sdrpp_debian_amd64/DEBIAN/control
echo Architecture: amd64 >> sdrpp_debian_amd64/DEBIAN/control
echo Description: Predator RF — solo SIGINT sensing platform >> sdrpp_debian_amd64/DEBIAN/control
echo Depends: $2 >> sdrpp_debian_amd64/DEBIAN/control

# Post-install: create system user and enable systemd service
cat > sdrpp_debian_amd64/DEBIAN/postinst << 'EOF'
#!/bin/sh
set -e

# Create the predator system user/group if they don't already exist.
# The service runs as this user for privilege isolation.
if ! getent group predator >/dev/null 2>&1; then
    groupadd --system predator
fi
if ! getent passwd predator >/dev/null 2>&1; then
    useradd --system \
            --gid predator \
            --no-create-home \
            --shell /usr/sbin/nologin \
            --home /var/lib/predator-rf \
            --comment "Predator RF daemon" \
            predator
fi

# Ensure the data and log directories are owned by the service user
chown -R predator:predator /var/lib/predator-rf /var/log/predator-rf || true

if [ "$1" = "configure" ] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    echo "predator-rfd installed. Enable with: systemctl enable --now predator-rfd"
fi
EOF
chmod 755 sdrpp_debian_amd64/DEBIAN/postinst

cat > sdrpp_debian_amd64/DEBIAN/prerm << 'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop predator-rfd 2>/dev/null || true
    systemctl disable predator-rfd 2>/dev/null || true
fi
EOF
chmod 755 sdrpp_debian_amd64/DEBIAN/prerm

# Copying files from cmake install
ORIG_DIR=$PWD
cd $1
make install DESTDIR=$ORIG_DIR/sdrpp_debian_amd64
cd $ORIG_DIR

# Install web assets (preview.html promoted as the live dashboard)
mkdir -p sdrpp_debian_amd64/usr/share/predator-rf/web
if [ -f web/index.html ]; then
    cp web/*.html web/*.js web/*.css sdrpp_debian_amd64/usr/share/predator-rf/web/ 2>/dev/null || true
fi
# preview.html is the canonical dashboard; serve it as index.html
if [ -f preview.html ] && [ ! -f sdrpp_debian_amd64/usr/share/predator-rf/web/index.html ]; then
    cp preview.html sdrpp_debian_amd64/usr/share/predator-rf/web/index.html
fi

# Operator data directory placeholder
mkdir -p sdrpp_debian_amd64/var/lib/predator-rf
mkdir -p sdrpp_debian_amd64/var/log/predator-rf
mkdir -p sdrpp_debian_amd64/etc/predator-rf
if [ -f deploy/predator-rf.env.example ]; then
    cp deploy/predator-rf.env.example sdrpp_debian_amd64/etc/predator-rf/predator-rf.env.example
fi

# Create package
echo Create package
dpkg-deb --build sdrpp_debian_amd64 predator-rf_1.1.0${BUILD_NO}_amd64.deb

# Cleanup
echo Cleanup
rm -rf sdrpp_debian_amd64
