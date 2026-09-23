# Certificates & Keys — Structure, Usage, Re-issuing

All cryptographic material of the testbed lives in a **private PKI outside the
Git repositories**: in the folder `../CA` (sibling folder of `fountainer_firmware`
and `fountainer_server`). Ground rules:

- **The CA never leaves this machine** and is not versioned in any Git repository.
  The repos contain only generated, git-ignored
  copies or embeddings.
- **No eFuses / no irreversible mechanisms** on the ESP32. The
  OTA signature check is a pure software check ("Signed App Verification
  without Secure Boot") — the chip remains fully reusable.
- Passwords of the testbed keys: see `../CA/PASSWORDS.md` (deliberately **not**
  in this repo). All testbed certificates are dummies and will be replaced for
  production use.

## 1. Overview: who holds which material?

```mermaid
flowchart LR
    subgraph CA["../CA (only on this machine)"]
        ROOT["Root CA<br/>root/private/ca.key.pem (RSA-4096)<br/>root/certs/ca.crt.pem"]
        SRV["Server certificate<br/>server/server.key.pem + .crt.pem"]
        ESP["Device certificate<br/>esp32/esp32.key(.plain).pem + .crt.pem"]
        OTA["OTA signing key<br/>ota_signing/ota_signing_key.pem (RSA-3072)"]
    end
    subgraph FW["fountainer_firmware"]
        CERTS["src/network/certs/<br/>ca.crt + client.crt + client.key<br/>(git-ignored, copies)"]
        GEN["certs_gen.c (generated)"]
    end
    subgraph SV["fountainer_server"]
        ENV["run_server.py reads<br/>TLS_CERT/TLS_KEY/TLS_CLIENT_CA<br/>relative: ../CA/…"]
    end
    ROOT -- signs --> SRV & ESP
    SRV --> ENV
    ESP -- copied --> CERTS --> GEN
    OTA -- signs images<br/>(tools/sign_firmware.py) --> FW
```

| Material | Location | Usage |
|---|---|---|
| Root CA (RSA-4096) | `../CA/root/` | issues server and device certificates; trust anchor for both sides |
| Server certificate (RSA-3072) | `../CA/server/` | TLS server identity (wss :8443, firmware download :8080); SAN: `SERVER_IP`, `127.0.0.1`, `server1.server.com`, `localhost` |
| Device certificate (RSA-3072) | `../CA/esp32/` | mTLS client identity of the ESP32 (CN = device_id) |
| OTA signing key (RSA-3072) | `../CA/ota_signing/` | signs every firmware image (Secure Boot V2 signature block, software check during OTA) |

**Why an unencrypted device key (`esp32.key.plain.pem`)?** The
esp_websocket_client does not support a key passphrase; protection in flash would
only be possible via flash encryption (= eFuse), which is ruled out by project rule.
The same applies to `ota_signing_key.pem` (build automation); a
password-protected archive copy (`.enc.pem`) of it exists.

## 2. How the material gets into firmware and server

- **Firmware:** `tools/ensure_network_json.py` (PlatformIO pre-script) reads
  `src/network/certs/{ca.crt.pem, client.crt.pem, client.key.pem}` and generates
  the git-ignored `src/network/certs_gen.c` from them. Missing files become
  empty strings — the firmware then falls back to unencrypted `ws://`
  (intended only for the very first commissioning). The three files are
  copies from `../CA` (`ca.crt.pem` ← `root/certs/`, `client.*` ← `esp32/`,
  where `client.key.pem` is the **plain** variant).
- **Server:** `run_server.py` receives the paths via environment variables —
  relative to the server directory:
  `TLS_CERT=../CA/server/server.crt.pem TLS_KEY=../CA/server/server.key.pem`
  `TLS_KEY_PASSWORD=<see PASSWORDS.md> TLS_CLIENT_CA=../CA/root/certs/ca.crt.pem`
- **OTA signature:** `tools/sign_firmware.py` (post-script, relative
  `../CA/ota_signing/ota_signing_key.pem`) appends the signature block to every
  built image. If the key is missing, the build warns and leaves the image
  unsigned (pure dev scenario — the device rejects it during OTA).

## 3. Procedures

All commands are executed in `../CA`; `<PW>` stands for the passwords from
`PASSWORDS.md`. The script reference for everything below is `../CA/gen_pki.sh` —
it is idempotent (creates only what is missing).

### 3.1 Set up the complete PKI from scratch

```bash
cd ../CA && ./gen_pki.sh
```
Creates root CA, server certificate, ESP32 certificate and OTA signing key with
the documented extensions (CA: `v3_ca`; server: `serverAuth` + SAN;
device: `clientAuth`). Validity window deliberately 2020–2040: an ESP32 without
time synchronisation rejects certificates that start "in the future".

### 3.2 Issue a new device certificate (additional device)

The CN **must** match the device's `device_id` (e.g.
`esp32-a1b2c3d4e5f6`) — the server checks the identity against the
client certificate.

```bash
cd ../CA
ID="esp32-NEWID"                        # = device_id of the new device
mkdir -p "$ID"
# 1) Key (encrypted) + unencrypted copy for embedding
openssl genrsa -aes256 -passout pass:<DEVICE_PW> -out "$ID/$ID.key.pem" 3072
openssl rsa -in "$ID/$ID.key.pem" -passin pass:<DEVICE_PW> -out "$ID/$ID.key.plain.pem"
chmod 600 "$ID"/*.pem
# 2) CSR with CN = device_id
openssl req -new -sha256 -key "$ID/$ID.key.pem" -passin pass:<DEVICE_PW> \
    -subj "/O=Melowsyne Unipessoal Lda/CN=$ID" -out "$ID/$ID.csr.pem"
# 3) Sign with the CA (clientAuth, same validity as the existing certs)
openssl ca -batch -config openssl.cnf -notext -md sha256 -passin pass:<CA_PW> \
    -startdate 20200101000000Z -enddate 20400101000000Z \
    -extensions v3_client -in "$ID/$ID.csr.pem" -out "$ID/$ID.crt.pem"
```

Then get it into the device:

```bash
cd ../fountainer_firmware
cp ../CA/root/certs/ca.crt.pem       src/network/certs/ca.crt.pem
cp ../CA/$ID/$ID.crt.pem             src/network/certs/client.crt.pem
cp ../CA/$ID/$ID.key.plain.pem       src/network/certs/client.key.pem
./build.sh        # pre-script embeds the new files automatically
```
Initial delivery via USB (`./flash.sh`); all subsequent updates run signed
via OTA. On the server side, register the device in `devices.json` (device_id +
HMAC key).

### 3.3 Renew the server certificate / different address

For a new hostname or new IP, first adjust the SAN list in
`../CA/openssl.cnf` (`[ server_alt ]`), then:

```bash
cd ../CA
rm server/server.crt.pem server/server.csr.pem     # key can stay
./gen_pki.sh                                        # re-issues only what is missing
```
Restart the server — the devices verify against the (unchanged) root CA and
accept the new certificate without a firmware change, as long as the
contacted address is in the SAN list.

### 3.4 Replace the OTA signing key (caution: order matters!)

The **public** part of the signing key is embedded in the firmware —
a running device only accepts images signed with the key that its CURRENT
firmware knows. Hence two stages:

1. Build a transition firmware that accepts **both** public keys (old + new),
   and roll it out via OTA signed with the **old** key.
2. Only then switch to the new key and retire the old one.

A direct swap would lock all field devices out of further OTAs
(then only USB helps).

### 3.5 Replace the root CA

Like 3.4, just one level higher: roll out a transition firmware with the old **and** the new
CA certificate in the truststore, switch the server to the new certificate,
then remove the old CA from the firmware. For the testbed, the
simpler route is often: device is reachable → new `certs/` copies → OTA.

## 4. Checklist before every commit

- `git status` must **never** show: `*.pem`, `src/network/certs/`,
  `certs_gen.c`, `network.json`, `network_json_gen.c` (all git-ignored —
  do not weaken the ignore list).
- Document new keys/passwords exclusively in `../CA/PASSWORDS.md`,
  never in repo files.
