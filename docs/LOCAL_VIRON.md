# A local Viron to watch

viron-strata only reads. To see something move, you need a Viron and a driver
that changes it. This page sets up a throwaway Viron on your own machine. Do
not point the driver at a Viron that holds data you care about: it creates an
environment and moves entities around in it.

## 1. PostgreSQL and Viron

Viron's own [`compose.yml`](https://github.com/Preponderous-Software/viron/blob/main/compose.yml)
starts PostgreSQL 12 and the service on port 9999. Without Docker, any
PostgreSQL 12+ works:

```
git clone https://github.com/Preponderous-Software/viron && cd viron
psql -h localhost -U postgres -f db-scripts/setup/create_sequences.sql
psql -h localhost -U postgres -f db-scripts/setup/create_tables.sql
export JAVA_HOME=/path/to/jdk-21
./mvnw -q -DskipTests package
head -c 48 /dev/urandom | base64 > /tmp/viron-jwt.secret
JWT_SECRET="$(cat /tmp/viron-jwt.secret)" USAGE_REPORTING_ENABLED=false \
  java -jar target/viron-*.jar \
  "--database.dbUrl=jdbc:postgresql://localhost:5432/postgres?stringtype=unspecified"
```

`?stringtype=unspecified` is needed for writes. Viron binds creation dates as
strings into `TIMESTAMP` columns, and without the setting PostgreSQL rejects
`POST /environments` and `POST /entities`. viron-usd's integration setup uses
the same setting.

The client needs a Viron that has `GET /api/v1/locations/placements`
(viron#235, commit `c96362b`, 2026-10-18). CI pins `bd4a1f8`.

## 2. A token

Viron accepts UserAuth JWTs (HS256, shared secret, `iss` = `userauth` by
default). For a Viron you started yourself with your own secret,
`tools/mint_dev_token.py` makes one that lasts a day:

```
export VIRON_TOKEN="$(python3 tools/mint_dev_token.py /tmp/viron-jwt.secret)"
```

## 3. Drive it and watch it

```
python3 tools/driver.py --ticks 300 --tps 3 &        # prints "environment <id>"
build/viron-strata --environment <id>
```

The driver takes `--grids`, `--grid-size`, `--entities`, `--ticks`, `--tps` and
`--seed`. Each tick it moves every entity to a free neighbouring cell. Every few
ticks it also takes one entity off the grid, places a new one, or sends one to
another grid.

`tools/integration.sh <build-dir>` runs the driver and the client together and
ends with `--verify`. CI runs the same script.
