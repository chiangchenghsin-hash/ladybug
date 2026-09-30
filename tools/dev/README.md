# Local VFS Endpoint Servers

`range_file_server.py` serves files with `HEAD` and byte-range `GET` support. This is useful for
testing VFS reads of Parquet files over `http://` and local S3-shaped `s3://` URLs.

Run the commands below from the repository root.

## HTTP

Start a local HTTP endpoint:

```bash
python3 tools/dev/range_file_server.py --bind 127.0.0.1 --port 8766 --root "$PWD"
```

Then test an HTTP init file:

```bash
printf 'match (a)-[b]->(c) return *;\n:quit\n' \
  | ./build/release/tools/shell/lbug \
      -i http://127.0.0.1:8766/dataset/demo-db/icebug-disk/schema.cypher
```

The range support matters because `python3 -m http.server` can serve the Cypher file, but it is not
enough for Parquet readers that issue byte-range requests.

## S3

The S3 filesystem uses HTTPS. Generate a local self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /private/tmp/lbug_s3_key.pem \
  -out /private/tmp/lbug_s3_cert.pem \
  -days 1 \
  -subj '/CN=127.0.0.1'
```

Start a local path-style S3-shaped endpoint. The `--strip-prefix icebug` flag maps requests for
`/icebug/...` back to files under the repository root.

```bash
python3 tools/dev/range_file_server.py \
  --bind 127.0.0.1 \
  --port 9443 \
  --root "$PWD" \
  --strip-prefix icebug \
  --cert /private/tmp/lbug_s3_cert.pem \
  --key /private/tmp/lbug_s3_key.pem
```

Configure the S3 extension to use the local endpoint:

```cypher
CALL s3_endpoint='127.0.0.1:9443';
CALL s3_url_style='path';
```

Then use this URL shape:

```text
s3://icebug/dataset/demo-db/icebug-disk/schema.cypher
```

For shell init testing, the S3 options must be available before the `-i s3://...` file is opened.
