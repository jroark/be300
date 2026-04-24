#!/usr/bin/env node

const fs = require("fs/promises");
const path = require("path");
const readline = require("readline");

const DEFAULT_START_URL = "https://www.be300.org/forums/index.php?action=forum";
const DEFAULT_OUT_DIR = path.join("build-host", "be300_forums");
const DEFAULT_PROFILE_DIR = path.join(DEFAULT_OUT_DIR, "profile");
const DEFAULT_KEYWORDS = [
  "boot",
  "bootloader",
  "rom",
  "nand",
  "restore",
  "recovery",
  "serial",
  "usb",
  "sync",
  "firmware",
  "loader",
  "safe mode",
  "debug",
  "kernel",
  "driver",
  "hardware",
  "cable",
  "flash",
];

function printHelp() {
  console.log(`Usage: node tools/scrape_be300_forums.js [options]

Options:
  --url <url>                     Forum landing page to scrape
  --out <dir>                     Output directory (default: ${DEFAULT_OUT_DIR})
  --connect-cdp <url>             Attach to an existing Chrome devtools endpoint
  --browser <auto|chrome|chromium> Browser channel to use (default: auto)
  --profile-dir <dir>             Persistent browser profile dir (default: ${DEFAULT_PROFILE_DIR})
  --headless[=true|false]         Launch Chromium headless or headed (default: false)
  --max-boards <n|all>            Maximum boards to visit (default: all)
  --max-topics-per-board <n|all>  Maximum topics to save per board (default: all)
  --keywords <csv>                Override ranking keywords
  --delay-ms <n>                  Delay between navigations in ms (default: 1200)
  --page-timeout-ms <n>           Per-navigation timeout in ms (default: 45000)
  --goto-retries <n>              Retries for slow or flaky page loads (default: 3)
  --goto-retry-delay-ms <n>       Delay between page load retries (default: 2500)
  --challenge-timeout-ms <n>      Auto-wait for Cloudflare/forum load in ms (default: 60000)
  -h, --help                      Show this help

Examples:
  npm install
  npx playwright install chromium
  npm run scrape:be300-forums
  npm run scrape:be300-forums -- --headless=false --max-boards 5
  node tools/scrape_be300_forums.js --connect-cdp http://127.0.0.1:9222
`);
}

function parseBoolean(value, fallback) {
  if (value === undefined) {
    return fallback;
  }

  const normalized = String(value).trim().toLowerCase();
  if (["1", "true", "yes", "on"].includes(normalized)) {
    return true;
  }
  if (["0", "false", "no", "off"].includes(normalized)) {
    return false;
  }
  throw new Error(`Invalid boolean value: ${value}`);
}

function parseInteger(value, flagName) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    throw new Error(`${flagName} must be a positive integer`);
  }
  return parsed;
}

function parseLimit(value, flagName) {
  const normalized = String(value).trim().toLowerCase();
  if (normalized === "all" || normalized === "0") {
    return null;
  }
  return parseInteger(value, flagName);
}

function parseArgs(argv) {
  const options = {
    url: DEFAULT_START_URL,
    outDir: DEFAULT_OUT_DIR,
    connectCdp: null,
    browser: "auto",
    profileDir: null,
    profileDirExplicit: false,
    headless: false,
    maxBoards: null,
    maxTopicsPerBoard: null,
    keywords: [...DEFAULT_KEYWORDS],
    delayMs: 1200,
    pageTimeoutMs: 45000,
    gotoRetries: 3,
    gotoRetryDelayMs: 2500,
    challengeTimeoutMs: 60000,
  };

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];

    if (arg === "-h" || arg === "--help") {
      printHelp();
      process.exit(0);
    }

    if (arg === "--headless") {
      options.headless = true;
      continue;
    }
    if (arg.startsWith("--headless=")) {
      options.headless = parseBoolean(arg.split("=", 2)[1], options.headless);
      continue;
    }
    if (arg === "--url") {
      options.url = argv[++i];
      continue;
    }
    if (arg === "--out") {
      options.outDir = argv[++i];
      continue;
    }
    if (arg === "--connect-cdp") {
      options.connectCdp = argv[++i];
      continue;
    }
    if (arg === "--browser") {
      options.browser = String(argv[++i]).trim().toLowerCase();
      continue;
    }
    if (arg === "--profile-dir") {
      options.profileDir = argv[++i];
      options.profileDirExplicit = true;
      continue;
    }
    if (arg === "--max-boards") {
      options.maxBoards = parseLimit(argv[++i], "--max-boards");
      continue;
    }
    if (arg === "--max-topics-per-board") {
      options.maxTopicsPerBoard = parseLimit(
        argv[++i],
        "--max-topics-per-board",
      );
      continue;
    }
    if (arg === "--keywords") {
      options.keywords = argv[++i]
        .split(",")
        .map((item) => item.trim().toLowerCase())
        .filter(Boolean);
      continue;
    }
    if (arg === "--delay-ms") {
      options.delayMs = parseInteger(argv[++i], "--delay-ms");
      continue;
    }
    if (arg === "--page-timeout-ms") {
      options.pageTimeoutMs = parseInteger(argv[++i], "--page-timeout-ms");
      continue;
    }
    if (arg === "--goto-retries") {
      options.gotoRetries = parseInteger(argv[++i], "--goto-retries");
      continue;
    }
    if (arg === "--goto-retry-delay-ms") {
      options.gotoRetryDelayMs = parseInteger(argv[++i], "--goto-retry-delay-ms");
      continue;
    }
    if (arg === "--challenge-timeout-ms") {
      options.challengeTimeoutMs = parseInteger(
        argv[++i],
        "--challenge-timeout-ms",
      );
      continue;
    }

    throw new Error(`Unknown argument: ${arg}`);
  }

  if (!options.keywords.length) {
    throw new Error("At least one keyword is required");
  }
  if (!["auto", "chrome", "chromium"].includes(options.browser)) {
    throw new Error("--browser must be one of: auto, chrome, chromium");
  }
  if (options.connectCdp) {
    try {
      new URL(options.connectCdp);
    } catch {
      throw new Error("--connect-cdp must be a valid URL, e.g. http://127.0.0.1:9222");
    }
  }
  if (!options.profileDirExplicit) {
    options.profileDir = path.join(options.outDir, "profile");
  }

  return options;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function pathExists(filePath) {
  try {
    await fs.access(filePath);
    return true;
  } catch {
    return false;
  }
}

async function ensureDir(dirPath) {
  await fs.mkdir(dirPath, { recursive: true });
}

async function detectBrowserChannel(requestedBrowser) {
  if (requestedBrowser !== "auto") {
    return requestedBrowser;
  }

  const chromePaths = [
    "/Applications/Google Chrome.app",
    path.join(process.env.HOME || "", "Applications/Google Chrome.app"),
  ];

  for (const chromePath of chromePaths) {
    if (chromePath && (await pathExists(chromePath))) {
      return "chrome";
    }
  }

  return "chromium";
}

async function uniqueFilePath(dirPath, baseName, extension) {
  let attempt = 0;
  while (true) {
    const suffix = attempt === 0 ? "" : `-${attempt}`;
    const candidate = path.join(dirPath, `${baseName}${suffix}.${extension}`);
    if (!(await pathExists(candidate))) {
      return candidate;
    }
    attempt += 1;
  }
}

function slugify(value) {
  return String(value)
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 80) || "page";
}

function normalizeUrl(rawUrl) {
  const url = new URL(rawUrl);
  url.hash = "";
  if (url.pathname.endsWith("/")) {
    url.pathname = url.pathname.slice(0, -1);
  }
  return url.toString();
}

function classifyForumUrl(rawUrl) {
  const info = parseForumPageInfo(rawUrl);
  if (info) {
    return info.kind;
  }
  const url = normalizeUrl(rawUrl);
  if (/action=forum/i.test(url)) {
    return "forum";
  }
  return "other";
}

function parseForumPageInfo(rawUrl) {
  const url = new URL(normalizeUrl(rawUrl));
  const haystack = `${url.pathname}${url.search}`;
  const match =
    haystack.match(/(?:[?&;]|\/)(board|topic)(?:=|,)(\d+)\.(\d+)/i) ||
    haystack.match(/index\.php\/(board|topic),(\d+)\.(\d+)/i);

  if (!match) {
    return null;
  }

  const kind = match[1].toLowerCase();
  const id = match[2];
  const offset = Number.parseInt(match[3], 10);
  const pageUrl = `${url.origin}${url.pathname}?${kind}=${id}.${offset}`;
  const rootUrl = `${url.origin}${url.pathname}?${kind}=${id}.0`;

  return {
    kind,
    id,
    offset,
    pageUrl,
    rootUrl,
    entityKey: `${kind}:${id}`,
    pageKey: `${kind}:${id}:${offset}`,
  };
}

function trimText(value, maxLength = 320) {
  const normalized = String(value || "").replace(/\s+/g, " ").trim();
  if (normalized.length <= maxLength) {
    return normalized;
  }
  return `${normalized.slice(0, maxLength - 3)}...`;
}

function scoreLink(link, keywords) {
  const titleText = trimText(link.text, 1000).toLowerCase();
  const contextText = `${trimText(link.snippet, 1000)} ${link.href}`.toLowerCase();
  const matches = [];
  let score = 0;

  for (const keyword of keywords) {
    if (!keyword) {
      continue;
    }
    if (titleText.includes(keyword)) {
      matches.push(keyword);
      score += 4;
      continue;
    }
    if (contextText.includes(keyword)) {
      matches.push(keyword);
      score += 1;
    }
  }

  return {
    score,
    matchedKeywords: [...new Set(matches)],
  };
}

function isJunkEntityLabel(text) {
  const normalized = trimText(text, 200).toLowerCase();
  return [
    "last post by",
    "subject",
    "started by",
    "replies",
    "views",
    "last post",
  ].includes(normalized);
}

function dedupeRankedEntities(links, expectedKind, keywords) {
  const byEntity = new Map();

  links.forEach((link, index) => {
    const info = parseForumPageInfo(link.href);
    if (!info || info.kind !== expectedKind) {
      return;
    }
    if (isJunkEntityLabel(link.text)) {
      return;
    }

    const ranking = scoreLink(link, keywords);
    const entry = {
      url: info.rootUrl,
      title: trimText(link.text, 200) || info.rootUrl,
      snippet: trimText(link.snippet),
      order: index,
      score: ranking.score,
      matchedKeywords: ranking.matchedKeywords,
      kind: info.kind,
      id: info.id,
      offset: info.offset,
      pageUrl: info.pageUrl,
      rootUrl: info.rootUrl,
      entityKey: info.entityKey,
    };

    const current = byEntity.get(info.entityKey);
    if (
      !current ||
      entry.score > current.score ||
      (entry.score === current.score && entry.title.length > current.title.length)
    ) {
      byEntity.set(info.entityKey, entry);
    }
  });

  return [...byEntity.values()].sort((a, b) => {
    if (b.score !== a.score) {
      return b.score - a.score;
    }
    return a.order - b.order;
  });
}

function dedupeRankedPages(links, expectedKind, keywords, options = {}) {
  const byPage = new Map();

  links.forEach((link, index) => {
    const info = parseForumPageInfo(link.href);
    if (!info || info.kind !== expectedKind) {
      return;
    }
    if (options.entityId && info.id !== options.entityId) {
      return;
    }
    if (options.rootOnly && info.offset !== 0) {
      return;
    }
    if (/[?&;]sort=/i.test(normalizeUrl(link.href))) {
      return;
    }

    const ranking = scoreLink(link, keywords);
    const entry = {
      url: info.pageUrl,
      title: trimText(link.text, 200) || info.pageUrl,
      snippet: trimText(link.snippet),
      order: index,
      score: ranking.score,
      matchedKeywords: ranking.matchedKeywords,
      kind: info.kind,
      id: info.id,
      offset: info.offset,
      pageUrl: info.pageUrl,
      rootUrl: info.rootUrl,
      entityKey: info.entityKey,
      pageKey: info.pageKey,
    };

    const current = byPage.get(info.pageKey);
    if (
      !current ||
      entry.score > current.score ||
      (entry.score === current.score && entry.title.length > current.title.length)
    ) {
      byPage.set(info.pageKey, entry);
    }
  });

  return [...byPage.values()].sort((a, b) => {
    if (a.id !== b.id) {
      return a.id.localeCompare(b.id, undefined, { numeric: true });
    }
    return a.offset - b.offset;
  });
}

function selectRankedLinks(candidates, limit) {
  if (!candidates.length) {
    return [];
  }
  if (limit == null) {
    return candidates;
  }

  const positivelyRanked = candidates.filter((item) => item.score > 0);
  const source = positivelyRanked.length ? positivelyRanked : candidates;
  return source.slice(0, limit);
}

function filterForumHostLinks(links) {
  return links.filter((link) => {
    try {
      const host = new URL(link.href).hostname.toLowerCase();
      return host === "www.be300.org" || host === "be300.org";
    } catch {
      return false;
    }
  });
}

function formatError(error) {
  return error && error.message ? error.message : String(error);
}

function isRetryableNavigationError(error) {
  const message = formatError(error).toLowerCase();
  return (
    message.includes("timeout") ||
    message.includes("net::err") ||
    message.includes("navigation") ||
    message.includes("target page, context or browser has been closed")
  );
}

async function waitForEnter(message) {
  if (!process.stdin.isTTY) {
    console.error(`${message} No interactive TTY detected; continuing to poll.`);
    return;
  }

  await new Promise((resolve) => {
    const rl = readline.createInterface({
      input: process.stdin,
      output: process.stdout,
    });
    rl.question(`${message}\nPress Enter once the page looks usable.\n`, () => {
      rl.close();
      resolve();
    });
  });
}

function snapshotLooksBlocked(snapshot, pageUrl) {
  const title = (snapshot.title || "").toLowerCase();
  const body = (snapshot.bodyText || "").toLowerCase();
  const url = (pageUrl || "").toLowerCase();

  if (
    title.includes("just a moment") ||
    title.includes("attention required")
  ) {
    return true;
  }

  return [
    "enable javascript and cookies to continue",
    "verify you are human",
    "checking your browser before accessing",
    "cloudflare",
  ].some((needle) => body.includes(needle)) || url.includes("__cf_chl");
}

async function captureDomSnapshot(page) {
  return page.evaluate(() => {
    const normalize = (value) => String(value || "").replace(/\s+/g, " ").trim();
    const shorten = (value, max) => {
      const text = normalize(value);
      if (text.length <= max) {
        return text;
      }
      return `${text.slice(0, max - 3)}...`;
    };

    const links = [];
    const seen = new Set();
    const candidates = document.querySelectorAll("a[href]");

    for (const anchor of candidates) {
      let href;
      try {
        href = new URL(anchor.getAttribute("href"), document.baseURI).toString();
      } catch {
        continue;
      }

      const text = normalize(anchor.textContent);
      if (!text) {
        continue;
      }

      const container = anchor.closest("tr, li, article, section, div, td");
      const snippet = shorten(container ? container.textContent : text, 320);
      const dedupeKey = `${href}\n${text}`;
      if (seen.has(dedupeKey)) {
        continue;
      }
      seen.add(dedupeKey);
      links.push({ href, text, snippet });
    }

    const headings = Array.from(
      document.querySelectorAll("h1, h2, h3, .catbg, .titlebg"),
    )
      .map((element) => normalize(element.textContent))
      .filter(Boolean)
      .slice(0, 12);

    return {
      title: document.title || "",
      bodyText: shorten(document.body ? document.body.innerText : "", 4000),
      headings,
      links,
    };
  });
}

async function waitForUsablePage(page, options, label, mode) {
  const deadline = Date.now() + options.challengeTimeoutMs;
  let lastSnapshot = null;

  while (Date.now() < deadline) {
    lastSnapshot = await captureDomSnapshot(page);
    const blocked = snapshotLooksBlocked(lastSnapshot, page.url());
    const hasForumLinks = lastSnapshot.links.some((link) => {
      const kind = classifyForumUrl(link.href);
      return kind === "board" || kind === "topic";
    });
    const hasGenericContent =
      lastSnapshot.bodyText.length > 0 ||
      lastSnapshot.title.length > 0 ||
      lastSnapshot.headings.length > 0 ||
      lastSnapshot.links.length > 0;
    const usable =
      !blocked &&
      (mode === "forum" ? hasForumLinks : hasGenericContent);

    if (usable) {
      return lastSnapshot;
    }

    await sleep(1000);
  }

  if (!options.headless) {
    await waitForEnter(
      `[scrape] ${label} still looks blocked or incomplete in Chromium.`,
    );

    const manualDeadline = Date.now() + options.challengeTimeoutMs;
    while (Date.now() < manualDeadline) {
      lastSnapshot = await captureDomSnapshot(page);
      const blocked = snapshotLooksBlocked(lastSnapshot, page.url());
      const hasForumLinks = lastSnapshot.links.some((link) => {
        const kind = classifyForumUrl(link.href);
        return kind === "board" || kind === "topic";
      });
      const hasGenericContent =
        lastSnapshot.bodyText.length > 0 ||
        lastSnapshot.title.length > 0 ||
        lastSnapshot.headings.length > 0 ||
        lastSnapshot.links.length > 0;
      const usable =
        !blocked &&
        (mode === "forum" ? hasForumLinks : hasGenericContent);

      if (usable) {
        return lastSnapshot;
      }
      await sleep(1000);
    }
  }

  const failureTitle = lastSnapshot?.title || "<no title>";
  const blockedHint =
    options.headless && snapshotLooksBlocked(lastSnapshot || {}, page.url())
      ? " Try rerunning with --headless=false and solve the Cloudflare check in Chromium."
      : "";
  throw new Error(
    `${label} never became usable. Last title: ${failureTitle} (${page.url()}).${blockedHint}`,
  );
}

async function tryCurrentPage(page, options, label, mode, timeoutMs) {
  const probeOptions = {
    ...options,
    challengeTimeoutMs: timeoutMs,
  };
  try {
    return await waitForUsablePage(page, probeOptions, label, mode);
  } catch {
    return null;
  }
}

async function navigateAndWait(page, url, options, label, mode) {
  let lastError = null;

  for (let attempt = 1; attempt <= options.gotoRetries; attempt += 1) {
    console.error(
      `[scrape] Opening ${label} page attempt ${attempt}/${options.gotoRetries}: ${url}`,
    );

    try {
      await page.goto(url, {
        waitUntil: "domcontentloaded",
        timeout: options.pageTimeoutMs,
      });
      const snapshot = await waitForUsablePage(page, options, label, mode);
      await sleep(options.delayMs);
      return snapshot;
    } catch (error) {
      lastError = error;

      const salvagedSnapshot = await tryCurrentPage(
        page,
        options,
        `${label} after navigation error`,
        mode,
        3000,
      );
      if (salvagedSnapshot) {
        console.error(
          `[scrape] ${label} recovered after navigation error: ${formatError(error)}`,
        );
        await sleep(options.delayMs);
        return salvagedSnapshot;
      }

      if (!isRetryableNavigationError(error) || attempt === options.gotoRetries) {
        throw error;
      }

      console.error(
        `[scrape] Retry ${attempt}/${options.gotoRetries} failed for ${label}: ${formatError(error)}`,
      );
      await sleep(options.gotoRetryDelayMs);
    }
  }

  throw lastError;
}

async function writeJson(filePath, value) {
  await fs.writeFile(filePath, `${JSON.stringify(value, null, 2)}\n`, "utf8");
}

async function flushArtifacts(outputDir, metadata, boardResults, topicResults) {
  await writeJson(path.join(outputDir, "boards.json"), {
    generatedAt: new Date().toISOString(),
    ...metadata,
    boards: boardResults,
  });

  await writeJson(path.join(outputDir, "topics.json"), {
    generatedAt: new Date().toISOString(),
    startUrl: metadata.startUrl,
    keywords: metadata.keywords,
    browser: metadata.browser,
    profileDir: metadata.profileDir,
    topics: topicResults,
  });
}

async function saveCurrentPage(page, outputDirs, label, withScreenshot) {
  const baseName = slugify(label);
  const htmlPath = await uniqueFilePath(outputDirs.pagesDir, baseName, "html");
  const html = await page.content();
  await fs.writeFile(htmlPath, html, "utf8");

  let screenshotPath = null;
  if (withScreenshot) {
    screenshotPath = await uniqueFilePath(outputDirs.pagesDir, baseName, "png");
    await page.screenshot({
      path: screenshotPath,
      fullPage: true,
    });
  }

  return {
    htmlPath,
    screenshotPath,
  };
}

function toRelativePath(baseDir, filePath) {
  if (!filePath) {
    return null;
  }
  return path.relative(baseDir, filePath);
}

async function loadPlaywright() {
  try {
    return require("playwright");
  } catch (error) {
    error.message = [
      "Playwright is not installed.",
      "Run `npm install` and `npx playwright install chromium`, then retry.",
      error.message,
    ].join(" ");
    throw error;
  }
}

async function launchBrowserContext(playwright, options) {
  const browserChannel = await detectBrowserChannel(options.browser);
  const profileDir = path.resolve(options.profileDir);
  await ensureDir(profileDir);

  const launchOptions = {
    headless: options.headless,
    channel: browserChannel === "chrome" ? "chrome" : undefined,
    viewport: { width: 1440, height: 1200 },
    locale: "en-US",
    ignoreHTTPSErrors: false,
    ignoreDefaultArgs: ["--enable-automation"],
    args: [
      "--disable-blink-features=AutomationControlled",
    ],
  };

  const context = await playwright.chromium.launchPersistentContext(
    profileDir,
    launchOptions,
  );

  await context.addInitScript(() => {
    Object.defineProperty(navigator, "webdriver", {
      get: () => undefined,
    });
    window.chrome = window.chrome || { runtime: {} };
  });

  return {
    context,
    browserChannel,
    profileDir,
    close: async () => {
      await context.close();
    },
  };
}

async function connectToExistingBrowser(playwright, options) {
  const browser = await playwright.chromium.connectOverCDP(options.connectCdp);
  let context = browser.contexts()[0];
  if (!context) {
    context = await browser.newContext({
      viewport: { width: 1440, height: 1200 },
      locale: "en-US",
    });
  }

  return {
    context,
    browserChannel: "cdp",
    profileDir: null,
    // Leave the user-opened browser running; process exit drops the CDP connection.
    close: async () => {},
  };
}

async function crawlEntityPages(
  page,
  seedEntry,
  expectedKind,
  options,
  outputDir,
  pagesDir,
  labelPrefix,
  mode,
) {
  const seedInfo = parseForumPageInfo(seedEntry.url || seedEntry.rootUrl || seedEntry.pageUrl);
  if (!seedInfo || seedInfo.kind !== expectedKind) {
    throw new Error(`Cannot crawl ${expectedKind}: invalid seed URL`);
  }

  const pending = [seedInfo.pageUrl];
  const queued = new Set([seedInfo.pageKey]);
  const visited = new Set();
  const discoveredLinks = [];
  const savedPages = [];
  const failedPages = [];

  while (pending.length) {
    const currentUrl = pending.shift();
    const currentInfo = parseForumPageInfo(currentUrl);
    if (!currentInfo || visited.has(currentInfo.pageKey)) {
      continue;
    }
    visited.add(currentInfo.pageKey);

    let snapshot;
    try {
      snapshot = await navigateAndWait(
        page,
        currentInfo.pageUrl,
        options,
        `${labelPrefix} page ${currentInfo.offset}`,
        mode,
      );
    } catch (error) {
      const errorText = formatError(error);
      console.error(
        `[scrape] Skipping ${labelPrefix} page ${currentInfo.offset} after repeated failure: ${errorText}`,
      );
      failedPages.push({
        offset: currentInfo.offset,
        url: currentInfo.pageUrl,
        error: errorText,
      });
      continue;
    }
    const pageSave = await saveCurrentPage(
      page,
      { pagesDir },
      `${labelPrefix}-page-${currentInfo.offset}`,
      false,
    );

    savedPages.push({
      offset: currentInfo.offset,
      url: currentInfo.pageUrl,
      savedHtml: toRelativePath(outputDir, pageSave.htmlPath),
      pageTitle: trimText(snapshot.title, 200),
    });

    const forumLinks = filterForumHostLinks(snapshot.links);
    discoveredLinks.push(...forumLinks);

    const nextPages = dedupeRankedPages(
      forumLinks,
      expectedKind,
      options.keywords,
      { entityId: currentInfo.id },
    );

    for (const nextPage of nextPages) {
      if (!visited.has(nextPage.pageKey) && !queued.has(nextPage.pageKey)) {
        pending.push(nextPage.pageUrl);
        queued.add(nextPage.pageKey);
      }
    }
  }

  savedPages.sort((a, b) => a.offset - b.offset);
  return {
    entityId: seedInfo.id,
    rootUrl: seedInfo.rootUrl,
    savedPages,
    failedPages,
    discoveredLinks,
  };
}

async function run() {
  const options = parseArgs(process.argv.slice(2));
  const outputDir = path.resolve(options.outDir);
  const pagesDir = path.join(outputDir, "pages");
  await ensureDir(pagesDir);

  const playwright = await loadPlaywright();
  const session = options.connectCdp
    ? await connectToExistingBrowser(playwright, options)
    : await launchBrowserContext(playwright, options);
  const { context, browserChannel, profileDir } = session;
  const existingPages = context.pages();
  const page = existingPages.length
    ? existingPages[existingPages.length - 1]
    : await context.newPage();

  const boardResults = [];
  const topicResults = [];
  let artifactMetadata = null;

  try {
    console.error(
      options.connectCdp
        ? `[scrape] Attached to existing browser via ${options.connectCdp}`
        : `[scrape] Using ${browserChannel} with profile ${profileDir}`,
    );
    let indexSnapshot = null;
    if (options.connectCdp) {
      indexSnapshot = await tryCurrentPage(
        page,
        options,
        "current attached page",
        "forum",
        3000,
      );
      if (indexSnapshot) {
        console.error(`[scrape] Reusing current attached page: ${page.url()}`);
      }
    }
    if (!indexSnapshot) {
      indexSnapshot = await navigateAndWait(
        page,
        options.url,
        options,
        "forum index",
        "forum",
      );
    }

    const savedIndex = await saveCurrentPage(
      page,
      { pagesDir },
      "be300-forum-index",
      true,
    );

    await context.storageState({
      path: path.join(outputDir, "storage-state.json"),
    });

    artifactMetadata = {
      startUrl: options.url,
      keywords: options.keywords,
      browser: browserChannel,
      profileDir: profileDir,
      forumIndexHtml: toRelativePath(outputDir, savedIndex.htmlPath),
      forumIndexScreenshot: toRelativePath(outputDir, savedIndex.screenshotPath),
    };
    await flushArtifacts(outputDir, artifactMetadata, boardResults, topicResults);

    const boardCandidates = dedupeRankedEntities(
      filterForumHostLinks(indexSnapshot.links),
      "board",
      options.keywords,
    );

    const selectedBoards = selectRankedLinks(boardCandidates, options.maxBoards);
    if (!selectedBoards.length) {
      throw new Error("No board links were found on the forum landing page.");
    }

    console.error(
      `[scrape] Crawling ${selectedBoards.length} board(s) from ${boardCandidates.length} discovered board(s).`,
    );

    for (const [boardIndex, board] of selectedBoards.entries()) {
      const boardCrawl = await crawlEntityPages(
        page,
        board,
        "board",
        options,
        outputDir,
        pagesDir,
        `board-${boardIndex + 1}-${board.title}`,
        "forum",
      );

      const topicCandidates = dedupeRankedEntities(
        boardCrawl.discoveredLinks,
        "topic",
        options.keywords,
      );
      const selectedTopics = selectRankedLinks(
        topicCandidates,
        options.maxTopicsPerBoard,
      );

      const boardRecord = {
        title: board.title,
        url: board.url,
        score: board.score,
        matchedKeywords: board.matchedKeywords,
        snippet: board.snippet,
        savedHtml: boardCrawl.savedPages[0]?.savedHtml || null,
        boardPageCount: boardCrawl.savedPages.length,
        savedPages: boardCrawl.savedPages,
        failedPageCount: boardCrawl.failedPages.length,
        failedPages: boardCrawl.failedPages,
        topicCandidateCount: topicCandidates.length,
        selectedTopicCount: selectedTopics.length,
      };
      boardResults.push(boardRecord);
      await flushArtifacts(outputDir, artifactMetadata, boardResults, topicResults);

      console.error(
        `[scrape] Board "${board.title}" yielded ${topicCandidates.length} discovered topic(s); crawling ${selectedTopics.length}.`,
      );

      for (const [topicIndex, topic] of selectedTopics.entries()) {
        const topicCrawl = await crawlEntityPages(
          page,
          topic,
          "topic",
          options,
          outputDir,
          pagesDir,
          `topic-${boardIndex + 1}-${topicIndex + 1}-${topic.title}`,
          "generic",
        );

        topicResults.push({
          boardTitle: board.title,
          title: topic.title,
          url: topic.url,
          score: topic.score,
          matchedKeywords: topic.matchedKeywords,
          snippet: topic.snippet,
          savedHtml: topicCrawl.savedPages[0]?.savedHtml || null,
          topicPageCount: topicCrawl.savedPages.length,
          savedPages: topicCrawl.savedPages,
          failedPageCount: topicCrawl.failedPages.length,
          failedPages: topicCrawl.failedPages,
        });
        await flushArtifacts(outputDir, artifactMetadata, boardResults, topicResults);
      }

      await flushArtifacts(outputDir, artifactMetadata, boardResults, topicResults);
    }

    console.error(
      `[scrape] Wrote ${boardResults.length} boards and ${topicResults.length} topics to ${outputDir}`,
    );
  } finally {
    await session.close();
  }
}

run().catch((error) => {
  console.error(`[scrape] ${error.message}`);
  process.exitCode = 1;
});
