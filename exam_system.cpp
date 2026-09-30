// Offline/Online Exam System v3 - console prototype (C++17)
// Build: g++ -std=c++17 -O2 -pthread exam_system.cpp -o exam
//
// Files (created next to the program):
//   admin.dat               key salt for file protection
//   students.txt            registered student IDs + login records (password hashes)
//   questions.txt           question bank (admin edits through the program)
//   config.txt              exam duration in minutes
//   answers_<id>.bin/.txt   student answers (protected; master only)
//   results.bin/.txt        all results (protected; master sees all, student sees own)
//   lock_<id>.txt           "exam window open" lock (heartbeat), stops double login
//   network_status.txt      1 = online, 0 = offline  (SIMULATION - see checkOnline())
//   server_<id>.txt         simulated server copy, written only while online
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <random>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <system_error>
#include <filesystem>
using namespace std;
namespace fs = std::filesystem;

static const string MASTER_ID = "Mikhael@123";
static const string ADMIN_ID  = "890mik";
static uint64_t g_key = 0;
static void (*g_onEof)() = nullptr;   // set while an exam is running

// ---------------------------------------------------------------- utilities
// Reads one line. If input is closed (EOF) it saves exam progress (if any) and exits cleanly
// instead of looping forever.
string ask(const string& p) {
    cout << p << flush; string s;
    if (!getline(cin, s)) {
        if (g_onEof) g_onEof();
        cout << "\nInput closed. Exiting.\n" << flush; _Exit(0);
    }
    return s;
}
long long nowSec() { return chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count(); }
string clean(string s) { for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' '; return s; }
string trim(const string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == string::npos ? "" : s.substr(a, b - a + 1); }
bool parseUInt(const string& s0, int& out, int maxv) {   // whole string must be digits, no overflow
    string s = trim(s0); if (s.empty() || s.size() > 9) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    out = stoi(s); return out <= maxv;
}
long long toLL(const string& s, long long def) { try { size_t p; long long v = stoll(s, &p); return p == s.size() ? v : def; } catch (...) { return def; } }
bool validId(const string& id) {   // safe for file names: no / \ : spaces etc.
    if (id.empty() || id.size() > 32) return false;
    for (unsigned char c : id) if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
vector<string> split(const string& s, char d = '|') {
    vector<string> out; string t; stringstream ss(s);
    while (getline(ss, t, d)) out.push_back(t);
    if (!s.empty() && s.back() == d) out.push_back("");
    return out;
}
uint64_t fnv(const string& s) { uint64_t h = 1469598103934665603ULL; for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; } return h; }
string hex64(uint64_t v) { stringstream ss; ss << hex << v; return ss.str(); }
string hashPw(const string& salt, const string& pw) { return hex64(fnv(salt + pw)) + hex64(fnv(pw + salt + "x")); }
string randomToken(int n) {
    static mt19937_64 rng(random_device{}() ^ (uint64_t)nowSec());
    const string ch = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    string s; for (int i = 0; i < n; i++) s += ch[rng() % ch.size()]; return s;
}
// One-time password generated FROM the id (+ random nonce + time), so it differs on every login.
string generatePw(const string& id) {
    string h = hex64(fnv(id + randomToken(8) + to_string(nowSec())));
    while (h.size() < 16) h = "0" + h;
    transform(h.begin(), h.end(), h.begin(), ::toupper);
    return h.substr(0, 4) + "-" + h.substr(4, 4);
}
string toHex(const string& d) { static const char* h = "0123456789abcdef"; string o; for (unsigned char c : d) { o += h[c >> 4]; o += h[c & 15]; } return o; }
string xorCrypt(string d) {  // keystream XOR: obfuscation, NOT strong crypto
    uint64_t s = g_key;
    for (char& c : d) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; c ^= (char)((s >> 33) & 0xFF); }
    return d;
}
// Atomic write: temp file + rename, and every step is checked (no more silent failures).
bool writeAtomic(const string& path, const string& data) {
    string tmp = path + ".tmp";
    { ofstream f(tmp, ios::binary | ios::trunc); if (!f) return false; f.write(data.data(), data.size()); f.flush(); if (!f) return false; }
    error_code ec; fs::rename(tmp, path, ec); return !ec;
}
static const string MAGIC = "EXAMv3\n";
bool writeSecure(const string& base, const string& text) {
    string enc = xorCrypt(MAGIC + text);
    bool a = writeAtomic(base + ".bin", enc), b = writeAtomic(base + ".txt", toHex(enc) + "\n");
    return a && b;
}
// returns false if the file is missing OR damaged/tampered; use fileExists() to tell them apart
bool readSecure(const string& base, string& text) {
    ifstream b(base + ".bin", ios::binary); if (!b) return false;
    string enc((istreambuf_iterator<char>(b)), istreambuf_iterator<char>());
    string dec = xorCrypt(enc);
    if (dec.compare(0, MAGIC.size(), MAGIC) != 0) return false;
    text = dec.substr(MAGIC.size()); return true;
}
bool fileExists(const string& p) { error_code ec; return fs::exists(p, ec); }
void quarantine(const string& base) {   // keep a damaged file instead of overwriting it
    string tag = "_corrupt_" + to_string(nowSec());
    for (const char* e : {".bin", ".txt"}) if (fileExists(base + e)) { error_code ec; fs::rename(base + e, base + tag + e, ec); }
    cout << "WARNING: " << base << " was damaged or modified. A copy was kept as " << base << tag << ".bin\n";
}
string timeStr(long long t) { time_t tt = t; char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", localtime(&tt)); return b; }
string fmtLeft(long long s) { if (s < 0) s = 0; char b[40]; snprintf(b, sizeof b, "%02lldh %02lldm %02llds", s / 3600, (s % 3600) / 60, s % 60); return b; }

// SIMULATION: reads network_status.txt (1/0). Replace with a real connection check to your server.
bool checkOnline() { ifstream f("network_status.txt"); int v = 1; if (f >> v) return v != 0; return true; }

void initKey() {
    string salt; { ifstream f("admin.dat"); getline(f, salt); }
    if (salt.empty()) { salt = randomToken(16); writeAtomic("admin.dat", salt + "\n"); }
    g_key = fnv(MASTER_ID + salt + "answers-key");
}
bool genLogin(const string& role, const string& fixedId) {
    string id = ask(role + " ID: ");
    if (id != fixedId) { cout << "Wrong ID.\n"; return false; }
    string pw = generatePw(id);
    cout << "Your one-time " << role << " password (new every login): " << pw << "\n";
    if (ask("Enter password: ") != pw) { cout << "Password mismatch.\n"; return false; }
    return true;
}

// ---------------------------------------------------------------- questions / config
struct Q { string text, opt[4]; char correct = 'A'; };
vector<Q> loadQ() {
    vector<Q> v; ifstream f("questions.txt"); string l;
    while (getline(f, l)) { auto p = split(l); if (p.size() < 6 || p[5].empty()) continue; Q q; q.text = p[0]; for (int i = 0; i < 4; i++) q.opt[i] = p[i + 1]; q.correct = p[5][0]; v.push_back(q); }
    return v;
}
bool saveQ(const vector<Q>& v) {
    stringstream ss; for (auto& q : v) ss << q.text << "|" << q.opt[0] << "|" << q.opt[1] << "|" << q.opt[2] << "|" << q.opt[3] << "|" << q.correct << "\n";
    return writeAtomic("questions.txt", ss.str());
}
int loadDuration() { ifstream f("config.txt"); int m = 30; f >> m; return (m > 0 && m <= 1440) ? m : 30; }
bool saveDuration(int m) { return writeAtomic("config.txt", to_string(m) + "\n"); }

// ---------------------------------------------------------------- students
// status: 0 = may take exam, 1 = in progress, 2 = completed (blocked until admin allows re-exam)
struct Student { string id, name, roll, pwHash; int status = 0; long long start = 0; };
vector<Student> loadStudents() {
    vector<Student> v; ifstream f("students.txt"); string l; int n = 0;
    while (getline(f, l)) {
        n++; if (l.empty()) continue;
        auto p = split(l);
        if (p.size() < 6 || !validId(p[0])) { cout << "WARNING: students.txt line " << n << " is damaged and was skipped.\n"; continue; }
        Student s; s.id = p[0]; s.name = p[1]; s.roll = p[2]; s.pwHash = p[5];
        long long st = toLL(p[3], -1); s.status = (st >= 0 && st <= 2) ? (int)st : 2;   // damaged status -> blocked (safe)
        s.start = toLL(p[4], 0);
        if (s.status != st) cout << "WARNING: students.txt line " << n << ": bad status for " << s.id << "; set to BLOCKED.\n";
        v.push_back(s);
    }
    return v;
}
bool saveStudents(const vector<Student>& v) {
    stringstream ss; for (auto& s : v) ss << s.id << "|" << s.name << "|" << s.roll << "|" << s.status << "|" << s.start << "|" << s.pwHash << "\n";
    return writeAtomic("students.txt", ss.str());
}
void seedDemo() {
    if (!fileExists("questions.txt")) {
        vector<Q> v;
        auto add = [&](string t, string a, string b, string c, string d, char k) { Q q; q.text = t; q.opt[0] = a; q.opt[1] = b; q.opt[2] = c; q.opt[3] = d; q.correct = k; v.push_back(q); };
        add("What is 12 + 8?", "18", "20", "22", "24", 'B');
        add("What is the capital of India?", "Mumbai", "New Delhi", "Kolkata", "Chennai", 'B');
        add("Which planet is called the Red Planet?", "Venus", "Jupiter", "Mars", "Saturn", 'C');
        add("H2O is the chemical formula of?", "Salt", "Water", "Oxygen", "Hydrogen", 'B');
        add("How many days are in a leap year?", "364", "365", "366", "367", 'C');
        saveQ(v);
    }
    if (!fileExists("students.txt")) {
        vector<Student> v; for (int i = 1; i <= 4; i++) { Student s; s.id = "STU00" + to_string(i); s.name = "Demo Student " + to_string(i); s.roll = "DEMO"; v.push_back(s); }
        saveStudents(v);
    }
    if (!fileExists("config.txt")) saveDuration(30);
}
bool studentLogin(Student& out, vector<Student>& all) {
    string id = trim(ask("Student ID: ")); if (id.empty()) return false;
    Student* s = nullptr; for (auto& x : all) if (x.id == id) s = &x;
    if (!s) { cout << "This ID is not registered. Ask the master to add it.\n"; return false; }
    string pw = generatePw(id);
    cout << "Your one-time password for this login: " << pw << "\n";
    if (ask("Enter password: ") != pw) { cout << "Password mismatch.\n"; return false; }
    s->pwHash = hashPw(id, pw + to_string(nowSec()));
    if (s->name.empty()) s->name = clean(ask("Full name: "));
    if (s->roll.empty()) s->roll = clean(ask("Roll no / class: "));
    saveStudents(all); out = *s; return true;
}

// ---------------------------------------------------------------- answers / results
struct Answer { char ch = 0; string mode; long long t = 0; };
map<int, Answer> g_ans;
bool g_online = true;
void syncIfOnline(const Student& s, const string& body) {
    bool now = checkOnline();
    if (now != g_online) { g_online = now; cout << "\n*** Connection " << (now ? "restored: switched to ONLINE" : "lost: switched to OFFLINE") << " mode. Answers are still being saved. ***\n"; }
    if (g_online) { ofstream f("server_" + s.id + ".txt", ios::trunc); f << toHex(xorCrypt(body)) << "\n"; }
}
bool saveAnswers(const Student& s) {
    stringstream ss; ss << s.id << "|" << s.name << "|" << s.roll << "|" << s.start << "\n";
    for (auto& [q, a] : g_ans) if (a.ch) ss << q << "|" << a.ch << "|" << a.mode << "|" << a.t << "\n";
    bool ok = writeSecure("answers_" + s.id, ss.str());
    if (ok) syncIfOnline(s, ss.str());
    return ok;
}
void loadAnswers(const Student& s) {
    g_ans.clear(); string base = "answers_" + s.id, t;
    if (!readSecure(base, t)) { if (fileExists(base + ".bin")) quarantine(base); return; }
    stringstream ss(t); string l; getline(ss, l);
    while (getline(ss, l)) { auto p = split(l); if (p.size() < 4 || p[1].empty()) continue; int q = (int)toLL(p[0], 0); if (q > 0) g_ans[q] = {p[1][0], p[2], toLL(p[3], 0)}; }
}
struct Result { string id, name, roll; int score, total; long long t; };
vector<Result> loadResults(bool& ok) {
    vector<Result> v; string t; ok = true;
    if (!fileExists("results.bin")) return v;
    if (!readSecure("results", t)) { ok = false; return v; }
    stringstream ss(t); string l;
    while (getline(ss, l)) { auto p = split(l); if (p.size() < 6) continue; v.push_back({p[0], p[1], p[2], (int)toLL(p[3], 0), (int)toLL(p[4], 0), toLL(p[5], 0)}); }
    return v;
}
bool saveResults(const vector<Result>& v) {
    stringstream ss; for (auto& r : v) ss << r.id << "|" << r.name << "|" << r.roll << "|" << r.score << "|" << r.total << "|" << r.t << "\n";
    return writeSecure("results", ss.str());
}
void printResult(const Result& r) { cout << r.id << " | " << r.name << " | " << r.roll << " | " << r.score << "/" << r.total << " | " << timeStr(r.t) << "\n"; }

// ---------------------------------------------------------------- one exam window per student
string lockName(const string& id) { return "lock_" + id + ".txt"; }
bool acquireLock(const string& id) {
    string n = lockName(id);
    if (fileExists(n)) {
        long long hb = 0; { ifstream f(n); f >> hb; }
        if (hb == 0) { error_code ec; auto age = fs::file_time_type::clock::now() - fs::last_write_time(n, ec); if (!ec && age < chrono::seconds(6)) return false; }
        else if (nowSec() - hb < 6) return false;              // heartbeat is fresh: window still open
        error_code ec; fs::remove(n, ec);                       // stale lock from a crashed window
    }
    FILE* fp = fopen(n.c_str(), "wx"); if (!fp) return false;   // 'x' = create only if it does not exist
    fprintf(fp, "%lld\n", nowSec()); fclose(fp); return true;
}
void heartbeat(const string& id) { writeAtomic(lockName(id), to_string(nowSec()) + "\n"); }
void releaseLock(const string& id) { error_code ec; fs::remove(lockName(id), ec); }
struct LockGuard { string id; ~LockGuard() { releaseLock(id); } };

// ---------------------------------------------------------------- exam (with timer thread)
mutex g_mx; atomic<bool> g_done{false};
Student g_s; vector<Student> g_allS; vector<Q> g_qs; bool g_saved = true;
chrono::steady_clock::time_point g_deadline;   // steady clock: changing the PC clock does not add time
long long leftSec() { auto ms = chrono::duration_cast<chrono::milliseconds>(g_deadline - chrono::steady_clock::now()).count(); return ms > 0 ? (ms + 999) / 1000 : 0; }

bool finishExam(bool auto_end) {  // caller holds g_mx. Returns true only if everything was saved.
    int score = 0;
    for (size_t i = 0; i < g_qs.size(); i++) { auto it = g_ans.find(i + 1); if (it != g_ans.end() && it->second.ch == g_qs[i].correct) score++; }
    bool ok = saveAnswers(g_s);
    bool rok; auto rs = loadResults(rok);
    if (!rok) quarantine("results");                       // never silently overwrite a damaged results file
    rs.push_back({g_s.id, g_s.name, g_s.roll, score, (int)g_qs.size(), nowSec()});
    ok = saveResults(rs) && ok;
    if (ok) { for (auto& x : g_allS) if (x.id == g_s.id) { x.status = 2; x.start = 0; } ok = saveStudents(g_allS); }
    if (ok) {
        g_done = true;
        cout << "\n" << (auto_end ? "Time is up! Your answers were saved automatically. " : "Answers saved. ")
             << "Exam finished. This ID is now blocked until the admin allows a re-exam.\n" << flush;
        return true;
    }
    cout << "\n*** SAVE FAILED (disk full / no permission?). Tell the invigilator. Your ID stays open; log in again to retry. ***\n" << flush;
    if (auto_end) g_done = true;
    return false;
}
void examEof() { lock_guard<mutex> lk(g_mx); if (!g_done) { saveAnswers(g_s); cout << "\nInput closed: progress saved, exam can be resumed.\n"; } releaseLock(g_s.id); }
void watchdog() {
    long long lastWarn = -1, lastBeat = 0;
    while (!g_done) {
        this_thread::sleep_for(chrono::milliseconds(200));
        long long left = leftSec(), now = nowSec();
        if (now != lastBeat) { lastBeat = now; heartbeat(g_s.id); }
        if (left <= 0) {
            lock_guard<mutex> lk(g_mx); if (g_done) return;
            cout << "\n*** COUNTDOWN REACHED 00h 00m 00s ***\n";
            finishExam(true); cout << "Exiting.\n" << flush; releaseLock(g_s.id); _Exit(0);
        }
        if ((left == 300 || left == 60 || left == 10) && left != lastWarn) { lastWarn = left; cout << "\n*** WARNING: " << fmtLeft(left) << " left ***\n" << flush; }
    }
}
void showHeader() {
    int answered = 0; vector<int> rem;
    for (size_t i = 1; i <= g_qs.size(); i++) { if (g_ans.count(i) && g_ans[i].ch) answered++; else rem.push_back(i); }
    cout << "\n==============================================\n"
         << " TIME LEFT: " << fmtLeft(leftSec()) << "   | Mode: " << (checkOnline() ? "ONLINE" : "OFFLINE") << "\n"
         << " Answered: " << answered << "   Remaining: " << rem.size() << " of " << g_qs.size() << "   " << (g_saved ? "[saved]" : "[UNSAVED changes]") << "\n";
    cout << " Question status:";
    for (size_t i = 1; i <= g_qs.size(); i++) cout << "  Q" << i << (g_ans.count(i) && g_ans[i].ch ? "[" + string(1, g_ans[i].ch) + "]" : "[ ]");
    cout << "\n Not answered yet:";
    if (rem.empty()) cout << " none";
    for (int r : rem) cout << " Q" << r;
    cout << "\n==============================================\n";
}
void runExam(Student s) {
    g_allS = loadStudents(); Student* rec = nullptr; for (auto& x : g_allS) if (x.id == s.id) rec = &x;
    if (!rec) { cout << "ID no longer registered.\n"; return; }
    if (rec->status == 2) { cout << "You have already taken the exam. Ask admin for a re-exam.\n"; return; }
    g_qs = loadQ(); if (g_qs.empty()) { cout << "No questions available. Contact admin.\n"; return; }
    if (!acquireLock(s.id)) { cout << "This ID is already open in another exam window. Close it first (if that window crashed, wait 10 seconds and retry).\n"; return; }
    LockGuard lg{s.id};
    if (rec->status == 0) {  // fresh attempt: archive old answers, start the clock
        for (const char* e : {".bin", ".txt"}) { string f = "answers_" + s.id + e; if (fileExists(f)) { error_code ec; fs::rename(f, "answers_" + s.id + "_old" + to_string(nowSec()) + e, ec); } }
        rec->status = 1; rec->start = nowSec();
        if (!saveStudents(g_allS)) { cout << "ERROR: cannot write students.txt (disk full / no permission). Exam not started.\n"; return; }
        g_ans.clear();
    }
    g_s = *rec; loadAnswers(g_s);
    g_online = checkOnline(); g_saved = true; g_done = false;
    long long remain = g_s.start + (long long)loadDuration() * 60 - nowSec();
    g_deadline = chrono::steady_clock::now() + chrono::seconds(remain > 0 ? remain : 0);
    if (remain <= 0) { cout << "\nYour exam time has already ended.\n"; lock_guard<mutex> lk(g_mx); finishExam(true); return; }
    cout << "\nExam started (" << (g_online ? "ONLINE" : "OFFLINE") << "). Time left: " << fmtLeft(leftSec()) << ".\n"
         << "Commands: question number = answer | V = save | X = exit exam (save first with V, then X)\n";
    g_onEof = examEof;
    thread wd(watchdog);
    while (true) {
        { lock_guard<mutex> lk(g_mx); if (g_done) break; showHeader(); }
        string c = trim(ask("Enter question no / V / X > "));
        if (c == "V" || c == "v") {
            lock_guard<mutex> lk(g_mx); if (g_done) break;
            if (saveAnswers(g_s)) { g_saved = true; cout << "Answers saved.\n"; } else cout << "*** SAVE FAILED - tell the invigilator. ***\n";
            continue;
        }
        if (c == "X" || c == "x") {
            { lock_guard<mutex> lk(g_mx); if (g_done) break; if (!g_saved) { cout << "Please SAVE first (V), then exit (X).\n"; continue; } }
            if (ask("Exit and submit? Unanswered questions stay blank (y/n): ") != "y") continue;
            lock_guard<mutex> lk(g_mx); if (g_done) break;
            if (!g_saved) { cout << "Changes since last save. Save first (V).\n"; continue; }
            if (finishExam(false)) break;
            continue;
        }
        int n = 0; if (!parseUInt(c, n, (int)g_qs.size()) || n < 1) { cout << "Invalid question number.\n"; continue; }
        Q& q = g_qs[n - 1]; cout << "\nQ" << n << ". " << q.text << "\n";
        for (int i = 0; i < 4; i++) cout << "  " << char('A' + i) << ") " << q.opt[i] << "\n";
        string a = trim(ask("Answer (A-D, blank = clear): "));
        lock_guard<mutex> lk(g_mx); if (g_done) break;
        if (a.empty()) g_ans.erase(n);
        else if (a.size() == 1 && toupper((unsigned char)a[0]) >= 'A' && toupper((unsigned char)a[0]) <= 'D') g_ans[n] = {(char)toupper((unsigned char)a[0]), checkOnline() ? "ON" : "OFF", nowSec()};
        else { cout << "Invalid option (type a single letter A-D).\n"; continue; }
        g_saved = false;
        if (!saveAnswers(g_s)) cout << "*** WARNING: could not write your answer to disk! ***\n";   // student still must press V before X
    }
    g_done = true; wd.join(); g_onEof = nullptr;
}

// ---------------------------------------------------------------- admin / master
void adminMenu() {
    while (true) {
        auto qs = loadQ();
        cout << "\n--- ADMIN --- (cannot open answers or results)\n1 Add question\n2 Delete question\n3 Update question\n4 List questions\n5 Set exam duration\n6 List students\n7 Allow re-exam (remove block)\n0 Logout\n";
        string c = trim(ask("> "));
        if (c == "1" || c == "3") {
            int n = -1; if (c == "3") { if (!parseUInt(ask("Question no: "), n, (int)qs.size()) || n < 1) { cout << "Invalid.\n"; continue; } n--; }
            Q q; q.text = clean(ask("Question: ")); for (int i = 0; i < 4; i++) q.opt[i] = clean(ask(string("Option ") + char('A' + i) + ": "));
            string k = trim(ask("Correct (A-D): ")); if (k.size() != 1 || toupper((unsigned char)k[0]) < 'A' || toupper((unsigned char)k[0]) > 'D') { cout << "Invalid key.\n"; continue; }
            q.correct = toupper((unsigned char)k[0]); if (n < 0) qs.push_back(q); else qs[n] = q;
            cout << (saveQ(qs) ? "Saved.\n" : "ERROR: could not write questions.txt\n");
        } else if (c == "2") {
            int n = 0; if (!parseUInt(ask("Question no: "), n, (int)qs.size()) || n < 1) { cout << "Invalid.\n"; continue; }
            qs.erase(qs.begin() + (n - 1)); cout << (saveQ(qs) ? "Deleted.\n" : "ERROR: could not write questions.txt\n");
        } else if (c == "4") { for (size_t i = 0; i < qs.size(); i++) cout << i + 1 << ". " << qs[i].text << " [key " << qs[i].correct << "]\n"; }
        else if (c == "5") { int m = 0; if (parseUInt(ask("Duration in minutes (1-1440): "), m, 1440) && m > 0) { cout << (saveDuration(m) ? "Set to " + fmtLeft(m * 60LL) + ".\n" : "ERROR: could not write config.txt\n"); } else cout << "Invalid.\n"; }
        else if (c == "6") { for (auto& s : loadStudents()) cout << s.id << " | " << s.name << " | " << s.roll << " | " << (s.status == 0 ? "not taken" : s.status == 1 ? "in progress" : "BLOCKED (completed)") << "\n"; }
        else if (c == "7") {
            auto all = loadStudents(); string id = trim(ask("Student ID: ")); bool ok = false;
            for (auto& s : all) if (s.id == id && s.status != 0) { s.status = 0; s.start = 0; ok = true; }
            if (ok && !saveStudents(all)) { cout << "ERROR: could not write students.txt\n"; continue; }
            cout << (ok ? "Block removed. Student can re-take the exam.\n" : "No blocked/in-progress student with that ID.\n");
        } else if (c == "0") return;
    }
}
void masterMenu() {
    while (true) {
        cout << "\n--- MASTER ---\n1 Examination result (all)\n2 View a student's answers\n3 Add student ID\n4 Remove student ID\n5 List students\n0 Logout\n";
        string c = trim(ask("> "));
        if (c == "1") {
            bool ok; auto rs = loadResults(ok);
            if (!ok) cout << "WARNING: results file is damaged or was modified - it cannot be read. (It is preserved, not overwritten.)\n";
            else if (rs.empty()) cout << "No results yet.\n";
            for (auto& r : rs) printResult(r);
        }
        else if (c == "2") { string t, id = trim(ask("Student ID: ")); if (validId(id) && readSecure("answers_" + id, t)) cout << t; else cout << "No readable answers file.\n"; }
        else if (c == "3") {
            auto all = loadStudents(); Student s; s.id = trim(ask("New student ID (letters, digits, _ - . only): "));
            if (!validId(s.id)) { cout << "Invalid ID. Use 1-32 letters/digits/_/-/. only (no spaces or slashes).\n"; continue; }
            bool dup = false; for (auto& x : all) if (x.id == s.id) dup = true; if (dup) { cout << "ID already exists.\n"; continue; }
            s.name = clean(ask("Name (blank = student enters at first login): ")); s.roll = clean(ask("Roll/class (blank = same): "));
            all.push_back(s); cout << (saveStudents(all) ? "Student " + s.id + " added. They can now take the exam.\n" : "ERROR: could not write students.txt\n");
        } else if (c == "4") {
            auto all = loadStudents(); string id = trim(ask("Student ID to remove: "));
            auto it = remove_if(all.begin(), all.end(), [&](const Student& x) { return x.id == id; });
            if (it == all.end()) { cout << "No such ID.\n"; continue; }
            if (ask("Remove " + id + "? Their results stay on record (y/n): ") != "y") continue;
            all.erase(it, all.end()); cout << (saveStudents(all) ? "Removed. This ID can no longer log in.\n" : "ERROR: could not write students.txt\n");
        } else if (c == "5") { for (auto& s : loadStudents()) cout << s.id << " | " << s.name << " | " << s.roll << " | " << (s.status == 0 ? "not taken" : s.status == 1 ? "in progress" : "completed") << "\n"; }
        else if (c == "0") return;
    }
}

// ---------------------------------------------------------------- main
int main() {
    initKey(); seedDemo();
    while (true) {
        cout << "\n===== EXAM SYSTEM =====\n1 Student: take exam\n2 Student: view my result\n3 Admin login\n4 Master login\n0 Exit\n";
        string c = trim(ask("> "));
        if (c == "1" || c == "2") {
            auto all = loadStudents(); Student s;
            if (!studentLogin(s, all)) continue;
            if (c == "1") runExam(s);
            else {
                bool ok; auto rs = loadResults(ok); Result* last = nullptr; for (auto& r : rs) if (r.id == s.id) last = &r;
                if (!ok) cout << "Results are temporarily unavailable. Please contact the master.\n";
                else if (last) printResult(*last); else cout << "No result available for your ID.\n";
            }
        } else if (c == "3") { if (genLogin("Admin", ADMIN_ID)) adminMenu(); }
        else if (c == "4") { if (genLogin("Master", MASTER_ID)) masterMenu(); }
        else if (c == "0") break;
    }
}
