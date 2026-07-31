import os

BASE_DIR = "Testing_CowNeck"
OUTPUT_DIR = "src"
OUTPUT_FILE = os.path.join(OUTPUT_DIR, "certs.c")



def read_file(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def is_private_key(content):
    return (
        "BEGIN PRIVATE KEY" in content or
        "BEGIN RSA PRIVATE KEY" in content
    )


def is_certificate(content):
    return "BEGIN CERTIFICATE" in content


def write_pem_as_c(f, var_name, pem_path):
    with open(pem_path, "rb") as pem:
        data = pem.read().decode("ascii", errors="ignore").splitlines()

    if not data:
        raise RuntimeError(f"{pem_path} is empty or unreadable")

    f.write(f"const uint8_t {var_name}[] =\n")
    for line in data:
        f.write(f"\"{line}\\n\"\n")
    f.write(";\n")
    f.write(f"const size_t {var_name}_len = sizeof({var_name}) - 1;\n\n")



def main():
    ca_cert = None
    client_cert = None
    private_key = None

    print("\nScanning files in:", BASE_DIR, "\n")

    for fname in os.listdir(BASE_DIR):
        path = os.path.join(BASE_DIR, fname)

        if not os.path.isfile(path):
            continue

        try:
            content = read_file(path)
        except Exception:
            continue

        print("Found:", fname)

        if is_private_key(content):
            private_key = path
            print("  → Identified as PRIVATE KEY")
            continue

        if is_certificate(content):
            if "certificate" in fname.lower():
                client_cert = path
                print("  → Identified as CLIENT CERT")
            else:
                # Prefer AmazonRootCA1 if multiple CA certs exist
                if ca_cert is None or "rootca1" in fname.lower():
                    ca_cert = path
                    print("  → Identified as CA CERT (selected)")
                else:
                    print("  → Identified as CA CERT (ignored)")

    if not ca_cert:
        raise FileNotFoundError("CA certificate not found")

    if not client_cert:
        raise FileNotFoundError("Client certificate not found")

    if not private_key:
        raise FileNotFoundError("Private key not found")

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write('#include "certs.h"\n\n')
        write_pem_as_c(f, "cacert_pem", ca_cert)
        write_pem_as_c(f, "client_pem", client_cert)
        write_pem_as_c(f, "client_key_pem", private_key)

    print("\n[OK] certs.c generated successfully\n")
    print(" CA Cert     :", os.path.basename(ca_cert))
    print(" Client Cert :", os.path.basename(client_cert))
    print(" Private Key :", os.path.basename(private_key))


if __name__ == "__main__":
    main()
