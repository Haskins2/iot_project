# Setting up local credentials (kept out of git)

Follow these steps to keep secret keys and credentials out of your repository while allowing other contributors to see the required configuration format.

1. Copy the example file

   - Copy `main/credentials.h.example` to `main/credentials.h`:

   ```bash
   cp main/credentials.h.example main/credentials.h
   ```

2. Fill in your secrets

   - Edit `main/credentials.h` and replace the placeholder values with your real Wi‑Fi SSID, Wi‑Fi password, and broker URI.

3. Ensure `main/credentials.h` is ignored by git

   - The repository already includes `.gitignore` entry for `/main/credentials.h`. Confirm with:

   ```bash
   git check-ignore -v main/credentials.h || echo "Not ignored"
   ```

4. If you accidentally committed secrets

   - Remove them from the commit history and stop tracking the file:

   ```bash
   git rm --cached main/credentials.h
   git commit -m "Remove local credentials from repo"
   ```

   - To scrub history use tools like `git filter-repo` or `bfg`, see their docs.

5. Alternatives (optional)

   - Use platform-specific secret storage (CI secrets, ESP‑IDF project config / sdkconfig, or environment variables) for production deployments.
   - For CI: set secret environment variables and inject them into the build rather than committing files.

6. Example `main/credentials.h` contents

```c
#define WIFI_SSID "MyNetwork"
#define WIFI_PASS "SuperSecret"
#define MQTT_URI "user@192.0.2.1"
```

That's it — build will use the values from `main/credentials.h` if present, otherwise the defaults in `main.c` will be used for local development.
