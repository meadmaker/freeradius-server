#!/bin/sh

# Allow setup script to work with homebrew too
export PATH="/usr/local/opt/openldap/libexec:$PATH"

# Clean out any existing DB
rm -rf /tmp/ldap2/db
# Create directory we can write DB files to
mkdir -p /tmp/ldap2/db/

# Change db location to /tmp as we can't write to /var
sed -i -e 's/\/var\/lib\/ldap/\/tmp\/ldap2\/db/' src/tests/salt-test-server/salt/ldap/base2.ldif

# Create a directory we can link schema files into
if [ -d /tmp/ldap2/schema ]; then
    echo "Schema dir already linked"
# Debian
elif [ -d /etc/ldap/schema ]; then
    ln -fs /etc/ldap/schema /tmp/ldap2/schema
# Redhat
elif [ -d /etc/openldap/schema ]; then
    ln -fs /etc/openldap/schema /tmp/ldap2/schema
# macOS (homebrew)
elif [ -d /usr/local/etc/openldap/schema ]; then
    ln -fs /usr/local/etc/openldap/schema /tmp/ldap2/schema
else
    echo "Can't locate OpenLDAP schema dir"
    exit 1
fi

# Clean out any old certificates
rm -rf /tmp/ldap2/certs
# Create certificate directory
mkdir -p /tmp/ldap2/certs

make -C raddb/certs

# Copy certificates - whilst not stricltly LDAP certs they work fine for these tests
cp raddb/certs/rsa/ca.pem /tmp/ldap2/certs/cacert.pem
cp raddb/certs/rsa/server.pem /tmp/ldap2/certs/servercert.pem
# OpenLDAP wants an un-encrypted key
openssl rsa -in raddb/certs/rsa/server.key -out /tmp/ldap2/certs/serverkey.pem -passin pass:whatever

# Start slapd
slapd -h "ldap://127.0.0.1:3891/ ldaps://127.0.0.1:6360" -f scripts/ci/ldap/slapd2.conf 2>&1 > /tmp/ldap/slapd.log &

# Wait for LDAP to start
sleep 1

# Add test data
count=0
while [ $count -lt 10 ] ; do
    if ldapadd -x -v -H ldap://127.0.0.1:3891/ -D "cn=admin,cn=config" -w secret -f src/tests/salt-test-server/salt/ldap/base2.ldif ; then
        break 2
    else
        echo "ldap add failed, retrying..."
        count=$((count+1))
        sleep 1
    fi
done

# Exit code gets overwritten, so we check for failure using count
if [ $count -eq 10 ]; then
    echo "Error configuring server"
    cat /tmp/ldap/slapd.log
    exit 1
fi
