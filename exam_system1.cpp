#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
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
#include <functional>
#include <memory>
#include <stdexcept>
#include <exception>
#include <system_error>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <conio.h>
#include <io.h>
#endif

using namespace std;
namespace fs = std::filesystem;

class AppContext;   

class ExamException : public runtime_error {
public:
    explicit ExamException(const string& m) : runtime_error(m) {}
    virtual const char* kind() const { return "Error"; }
};
class FileException : public ExamException {
public:
    using ExamException::ExamException;
    const char* kind() const override { return "File error"; }
};
class IntegrityException : public FileException {
public:
    using FileException::FileException;
    const char* kind() const override { return "Integrity error"; }
};
class ValidationException : public ExamException {
public:
    using ExamException::ExamException;
    const char* kind() const override { return "Invalid input"; }
};
class AuthException : public ExamException {
public:
    using ExamException::ExamException;
    const char* kind() const override { return "Login failed"; }
};
class LockException : public ExamException {
public:
    using ExamException::ExamException;
    const char* kind() const override { return "Exam window"; }
};

template <typename T> bool inRange(T v, T lo, T hi) { return v >= lo && v <= hi; }


class Util {
public:
    static long long nowSec() { return chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count(); }
    static string clean(string s) { for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' '; return s; }
    static string trim(const string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == string::npos ? "" : s.substr(a, b - a + 1); }
    static string toLower(string s) { transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); }); return s; }
    static bool parseUInt(const string& s0, int& out, int maxv) {
        string s = trim(s0); if (s.empty() || s.size() > 9) return false;
        for (char c : s) if (!isdigit((unsigned char)c)) return false;
        out = stoi(s); return out <= maxv;
    }
    static long long toLL(const string& s, long long def) { try { size_t p; long long v = stoll(s, &p); return p == s.size() ? v : def; } catch (...) { return def; } }
    static bool validId(const string& id) {
        if (id.empty() || id.size() > 32) return false;
        for (unsigned char c : id) if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@')) return false;
        return true;
    }
    static vector<string> split(const string& s, char d = '|') {
        vector<string> out; string t; stringstream ss(s);
        while (getline(ss, t, d)) out.push_back(t);
        if (!s.empty() && s.back() == d) out.push_back("");
        return out;
    }
    static uint64_t fnv(const string& s) { uint64_t h = 1469598103934665603ULL; for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; } return h; }
    static string hex64(uint64_t v) { stringstream ss; ss << hex << v; return ss.str(); }
    static string toHex(const string& d) { static const char* h = "0123456789abcdef"; string o; for (unsigned char c : d) { o += h[c >> 4]; o += h[c & 15]; } return o; }
    static string fromHex(const string& hexStr) {
        string out = "";
        for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
            unsigned int byteVal;
            stringstream ss;
            ss << hex << hexStr.substr(i, 2);
            ss >> byteVal;
            out.push_back((char)byteVal);
        }
        return out;
    }
    static string randomToken(int n) {
        static mt19937_64 rng(random_device{}() ^ (uint64_t)nowSec());
        const string ch = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        string s; for (int i = 0; i < n; i++) s += ch[rng() % ch.size()]; return s;
    }
    static bool fileExists(const string& p) { error_code ec; return fs::exists(p, ec); }
    static bool writeAtomic(const string& path, const string& data) {
        string tmp = path + ".tmp";
        { ofstream f(tmp, ios::binary | ios::trunc); if (!f) return false; f.write(data.data(), data.size()); f.flush(); if (!f) return false; }
        error_code ec; fs::rename(tmp, path, ec); return !ec;
    }
    static string timeStr(long long t) { time_t tt = t; char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", localtime(&tt)); return b; }
    static string fmtLeft(long long s) {
        if (s < 0) s = 0;
        ostringstream o;
        o << setfill('0') << setw(2) << s / 3600 << "h " << setw(2) << (s % 3600) / 60 << "m " << setw(2) << s % 60 << "s";
        return o.str();
    }
};

struct HttpResponse {
    int statusCode = 0;
    string body;
    bool ok = false;
};

class HttpClient {
public:
    static bool parseUrl(const string& url, string& host, int& port, string& path, bool& isHttps) {
        string u = url; isHttps = false; port = 80; path = "/";
        if (u.rfind("https://", 0) == 0) { isHttps = true; port = 443; u = u.substr(8); }
        else if (u.rfind("http://", 0) == 0) { isHttps = false; port = 80; u = u.substr(7); }
        size_t slash = u.find('/');
        string hostPort = (slash == string::npos) ? u : u.substr(0, slash);
        if (slash != string::npos) path = u.substr(slash);
        size_t colon = hostPort.find(':');
        if (colon != string::npos) {
            host = hostPort.substr(0, colon);
            try { port = stoi(hostPort.substr(colon + 1)); } catch (...) {}
        } else host = hostPort;
        if (host == "localhost") host = "127.0.0.1";
        return !host.empty();
    }

    static HttpResponse get(const string& fullUrl) {
        HttpResponse res;
#ifdef _WIN32
        string host, path; int port = 80; bool isHttps = false;
        if (!parseUrl(fullUrl, host, port, path, isHttps)) return res;
        HINTERNET hSession = WinHttpOpen(L"ExamClient/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return res;
        WinHttpSetTimeouts(hSession, 500, 500, 500, 500);
        wstring wHost(host.begin(), host.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return res; }
        wstring wPath(path.begin(), path.end());
        DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                if (WinHttpReceiveResponse(hRequest, NULL)) {
                    DWORD dwStatusCode = 0, dwSize = sizeof(dwStatusCode);
                    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
                    res.statusCode = (int)dwStatusCode;
                    res.ok = (res.statusCode >= 200 && res.statusCode < 300);
                    DWORD dwDownloaded = 0;
                    do {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                        string buf(dwSize, 0);
                        if (WinHttpReadData(hRequest, &buf[0], dwSize, &dwDownloaded)) { buf.resize(dwDownloaded); res.body += buf; }
                    } while (dwSize > 0);
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
#endif
        return res;
    }

    static HttpResponse post(const string& fullUrl, const string& jsonBody) {
        HttpResponse res;
#ifdef _WIN32
        string host, path; int port = 80; bool isHttps = false;
        if (!parseUrl(fullUrl, host, port, path, isHttps)) return res;
        HINTERNET hSession = WinHttpOpen(L"ExamClient/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return res;
        WinHttpSetTimeouts(hSession, 1000, 1000, 1000, 1000);
        wstring wHost(host.begin(), host.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return res; }
        wstring wPath(path.begin(), path.end());
        DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hRequest) {
            wstring headers = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.length(), (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(), (DWORD)jsonBody.length(), 0)) {
                if (WinHttpReceiveResponse(hRequest, NULL)) {
                    DWORD dwStatusCode = 0, dwSize = sizeof(dwStatusCode);
                    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
                    res.statusCode = (int)dwStatusCode;
                    res.ok = (res.statusCode >= 200 && res.statusCode < 300);
                    DWORD dwDownloaded = 0;
                    do {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                        string buf(dwSize, 0);
                        if (WinHttpReadData(hRequest, &buf[0], dwSize, &dwDownloaded)) { buf.resize(dwDownloaded); res.body += buf; }
                    } while (dwSize > 0);
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
#endif
        return res;
    }

    static string extractJson(const string& json, const string& key) {
        string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == string::npos) return "";
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\"')) {
            if (json[pos] == '\"') { pos++; break; }
            pos++;
        }
        size_t end = json.find('\"', pos);
        if (end == string::npos) return "";
        return json.substr(pos, end - pos);
    }

    static bool extractJsonBool(const string& json, const string& key) {
        string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == string::npos) return false;
        pos += search.size();
        while (pos < json.size() && json[pos] == ' ') pos++;
        return json.compare(pos, 4, "true") == 0;
    }
};

class ServerConfig {
public:
    static string getUrl() {
        ifstream f("server_config.txt");
        string l;
        while (getline(f, l)) {
            l = Util::trim(l);
            if (l.empty() || l[0] == '#') continue;
            if (l.rfind("SERVER_URL=", 0) == 0) return Util::trim(l.substr(11));
        }
        return "http://localhost:8000";
    }
};

class Network {
    static inline atomic<bool> s_simulatedOffline{false};
    static inline atomic<long long> s_lastCheck{0};
    static inline atomic<bool> s_lastResult{false};
public:
    static void toggleSimulation() {
        s_simulatedOffline = !s_simulatedOffline;
        s_lastCheck = 0;
    }
    static bool isSimulatedOffline() { return s_simulatedOffline; }
    static bool isOnline(bool forceCheck = false) {
        if (s_simulatedOffline) return false;
        long long now = Util::nowSec();
        if (!forceCheck && (now - s_lastCheck.load() < 2)) {
            return s_lastResult.load();
        }
        string url = ServerConfig::getUrl() + "/api/status";
        HttpResponse res = HttpClient::get(url);
        s_lastResult = res.ok;
        s_lastCheck = now;
        return res.ok;
    }
};

class AuditLog {
public:
    static void write(const string& event) {
        ofstream f("audit.log", ios::app);
        if (f) f << Util::timeStr(Util::nowSec()) << " | " << event << "\n";
        // Also send to central server if online
        string url = ServerConfig::getUrl() + "/api/log_event";
        string payload = "{\"event\": \"" + event + "\"}";
        HttpClient::post(url, payload);
    }
    static string tail(streamoff maxBytes) {
        ifstream f("audit.log", ios::binary | ios::ate);
        if (!f) throw FileException("audit.log does not exist yet or cannot be opened");
        streamoff size = f.tellg();
        streamoff start = size > maxBytes ? size - maxBytes : 0;
        f.seekg(start, ios::beg);
        string s((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        if (f.bad()) throw FileException("error while reading audit.log");
        if (start > 0) { size_t nl = s.find('\n'); if (nl != string::npos) s.erase(0, nl + 1); }
        return s;
    }
};

class IProtector {
public:
    virtual ~IProtector() = default;
    virtual string encode(string data) const = 0;
    virtual string decode(string data) const { return encode(std::move(data)); }
};
class XorProtector : public IProtector {
    uint64_t key_;
public:
    explicit XorProtector(uint64_t key) : key_(key) {}
    string encode(string d) const override {
        uint64_t s = key_;
        for (char& c : d) { s = s * 6364136223846793005ULL + 1442695040888963407ULL; c ^= (char)((s >> 33) & 0xFF); }
        return d;
    }
};

class IFileStorage {
public:
    virtual ~IFileStorage() = default;
    virtual bool exists(const string& name) const = 0;
    virtual bool read(const string& name, string& text) = 0;
    virtual bool write(const string& name, const string& text) = 0;
};
class PlainStorage : public IFileStorage {
public:
    bool exists(const string& name) const override { return Util::fileExists(name); }
    bool read(const string& name, string& text) override {
        ifstream f(name, ios::binary); if (!f) return false;
        text.assign((istreambuf_iterator<char>(f)), istreambuf_iterator<char>()); return true;
    }
    bool write(const string& name, const string& text) override { return Util::writeAtomic(name, text); }
};
class SecureStorage : public IFileStorage {
    const IProtector& prot_;
    static const string MAGIC;
public:
    explicit SecureStorage(const IProtector& p) : prot_(p) {}
    bool exists(const string& base) const override { return Util::fileExists(base + ".bin"); }
    bool write(const string& base, const string& text) override {
        string enc = prot_.encode(MAGIC + text);
        bool a = Util::writeAtomic(base + ".bin", enc), b = Util::writeAtomic(base + ".txt", Util::toHex(enc) + "\n");
        return a && b;
    }
    bool read(const string& base, string& text) override {
        ifstream b(base + ".bin", ios::binary); if (!b) return false;
        string enc((istreambuf_iterator<char>(b)), istreambuf_iterator<char>());
        string dec = prot_.decode(enc);
        if (dec.compare(0, MAGIC.size(), MAGIC) != 0) return false;
        text = dec.substr(MAGIC.size()); return true;
    }
    void quarantine(const string& base) {
        string tag = "_corrupt_" + to_string(Util::nowSec());
        for (const char* e : {".bin", ".txt"}) if (Util::fileExists(base + e)) { error_code ec; fs::rename(base + e, base + tag + e, ec); }
        cout << "WARNING: " << base << " was damaged or modified. A copy was kept as " << base << tag << ".bin\n";
    }
};
const string SecureStorage::MAGIC = "EXAMv5\n";

class ISerializable {
public:
    virtual ~ISerializable() = default;
    virtual string toLine() const = 0;
    virtual bool fromLine(const string& line) = 0;
    virtual string warning() const { return ""; }
};

template <typename T>
class Repository {
    vector<T> items_;
    IFileStorage& store_;
    string name_;
public:
    Repository(IFileStorage& store, string name) : store_(store), name_(std::move(name)) {}
    void add(const T& x) { items_.push_back(x); }
    T& at(size_t i) { return items_.at(i); }
    void removeAt(size_t i) { items_.erase(items_.begin() + (long)i); }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    void clear() { items_.clear(); }
    const vector<T>& items() const { return items_; }
    typename vector<T>::iterator begin() { return items_.begin(); }
    typename vector<T>::iterator end() { return items_.end(); }
    typename vector<T>::const_iterator begin() const { return items_.begin(); }
    typename vector<T>::const_iterator end() const { return items_.end(); }
    template <typename Pred> T* find(Pred p) {
        auto it = find_if(items_.begin(), items_.end(), p); return it == items_.end() ? nullptr : &*it;
    }
    template <typename Pred> size_t removeIf(Pred p) {
        auto it = remove_if(items_.begin(), items_.end(), p); size_t n = (size_t)(items_.end() - it); items_.erase(it, items_.end()); return n;
    }
    string serialize() const { string s; for (const T& x : items_) s += x.toLine() + "\n"; return s; }
    void load() {
        items_.clear();
        if (!store_.exists(name_)) return;
        string text;
        if (!store_.read(name_, text)) throw IntegrityException(name_ + " is damaged or was modified");
        stringstream ss(text); string l; int n = 0;
        while (getline(ss, l)) {
            n++; if (l.empty()) continue;
            T item;
            if (!item.fromLine(l)) { cout << "WARNING: " << name_ << " line " << n << " is damaged and was skipped.\n"; continue; }
            string w = item.warning(); if (!w.empty()) cout << "WARNING: " << name_ << " line " << n << ": " << w << "\n";
            items_.push_back(item);
        }
    }
    bool save() { return store_.write(name_, serialize()); }
};

class Question : public ISerializable {
    string text_;
    string opt_[4];
    string correct_;   
    char type_;        
public:
    Question() : correct_("A"), type_('M') {}
    Question(string t, string a, string b, string c, string d, char key = 'A')
        : text_(std::move(t)), opt_{std::move(a), std::move(b), std::move(c), std::move(d)}, correct_(string(1, (char)toupper((unsigned char)key))), type_('M') {}
    Question(string t, string sampleAnswer)
        : text_(std::move(t)), correct_(std::move(sampleAnswer)), type_('W') {}

    bool isWritten() const { return type_ == 'W' || (opt_[0].empty() && opt_[1].empty()); }
    char type() const { return isWritten() ? 'W' : 'M'; }
    void setType(char t) { type_ = t; }

    Question& setText(string t) { this->text_ = std::move(t); return *this; }
    Question& setOption(int i, string o) {
        if (!inRange(i, 0, 3)) throw ValidationException("Option number must be 0-3");
        opt_[i] = std::move(o); return *this;
    }
    Question& setCorrect(const string& s) {
        if (!isWritten() && (s.size() != 1 || !inRange((char)toupper((unsigned char)s[0]), 'A', 'D')))
            throw ValidationException("Invalid option key (must be A-D).");
        correct_ = s; return *this;
    }
    const string& text() const { return text_; }
    const string& option(int i) const { return opt_[i]; }
    const string& correct() const { return correct_; }

    string toLine() const override {
        return text_ + "|" + opt_[0] + "|" + opt_[1] + "|" + opt_[2] + "|" + opt_[3] + "|" + correct_ + "|" + type();
    }
    bool fromLine(const string& l) override {
        auto p = Util::split(l); if (p.size() < 6 || p[5].empty()) return false;
        text_ = p[0];
        for (int i = 0; i < 4; i++) opt_[i] = p[i + 1];
        correct_ = p[5];
        type_ = (p.size() >= 7 && !p[6].empty()) ? p[6][0] : ((opt_[0].empty() && opt_[1].empty()) ? 'W' : 'M');
        return true;
    }
};

struct Answer {
    char choice = 0;      
    string text = "";     
    string mode = "";     
    long long t = 0;
    bool isWritten = false;
};

class AnswerSheet {
    Answer* slots_; size_t n_;
    void check(size_t q) const { if (q < 1 || q > n_) throw ValidationException("Question number out of range"); }
public:
    explicit AnswerSheet(size_t n = 0) : slots_(n ? new Answer[n] : nullptr), n_(n) {}
    AnswerSheet(const AnswerSheet& o) : slots_(o.n_ ? new Answer[o.n_] : nullptr), n_(o.n_) { copy(o.slots_, o.slots_ + n_, slots_); }
    AnswerSheet& operator=(const AnswerSheet& o) { if (this != &o) { AnswerSheet tmp(o); swap(slots_, tmp.slots_); swap(n_, tmp.n_); } return *this; }
    ~AnswerSheet() { delete[] slots_; }

    size_t size() const { return n_; }
    bool answered(size_t q) const {
        if (q < 1 || q > n_) return false;
        const Answer& a = slots_[q - 1];
        return a.choice != 0 || !a.text.empty();
    }
    const Answer& get(size_t q) const { check(q); return slots_[q - 1]; }
    void setChoice(size_t q, char ch, string mode, long long t) { check(q); slots_[q - 1] = Answer{ch, "", std::move(mode), t, false}; }
    void setText(size_t q, string txt, string mode, long long t) { check(q); slots_[q - 1] = Answer{0, std::move(txt), std::move(mode), t, true}; }
    
    // Explicit Clear Answer command
    void clear(size_t q) { check(q); slots_[q - 1] = Answer(); }

    int answeredCount() const { int c = 0; for (size_t i = 1; i <= n_; i++) if (answered(i)) c++; return c; }
    string summary(size_t q) const {
        if (!answered(q)) return "[ ]";
        const Answer& a = slots_[q - 1];
        if (a.isWritten) return "[Written: " + (a.text.size() > 7 ? a.text.substr(0, 6) + ".." : a.text) + "]";
        return "[" + string(1, a.choice) + "]";
    }
};

class Result : public ISerializable {
    string id_, name_, roll_; int score_, total_; long long t_;
public:
    Result() : score_(0), total_(0), t_(0) {}
    Result(string id, string name, string roll, int score, int total, long long t)
        : id_(std::move(id)), name_(std::move(name)), roll_(std::move(roll)), score_(score), total_(total), t_(t) {}
    const string& id() const { return id_; }
    int score() const { return score_; }
    string toLine() const override { return id_ + "|" + name_ + "|" + roll_ + "|" + to_string(score_) + "|" + to_string(total_) + "|" + to_string(t_); }
    bool fromLine(const string& l) override {
        auto p = Util::split(l); if (p.size() < 6) return false;
        id_ = p[0]; name_ = p[1]; roll_ = p[2]; score_ = (int)Util::toLL(p[3], 0); total_ = (int)Util::toLL(p[4], 0); t_ = Util::toLL(p[5], 0); return true;
    }
    friend ostream& operator<<(ostream& os, const Result& r);
};
ostream& operator<<(ostream& os, const Result& r) {
    return os << r.id_ << " | " << r.name_ << " | " << r.roll_ << " | " << r.score_ << "/" << r.total_ << " | " << Util::timeStr(r.t_);
}

class Console {
    static function<void()> s_onEof;
public:
    static void setEofHook(function<void()> f) { s_onEof = std::move(f); }
    static string ask(const string& p) {
        cout << p << flush; string s;
        if (!getline(cin, s)) {
            if (s_onEof) s_onEof();
            cout << "\nInput closed. Exiting.\n" << flush; _Exit(0);
        }
        return s;
    }

    static string askLive(const string& prompt, chrono::steady_clock::time_point deadline, bool isOnline) {
#ifdef _WIN32
        if (!_isatty(_fileno(stdin))) {
            return ask(prompt);
        }
#endif
        string input = "";
        long long lastSec = -1;
        while (true) {
            auto now = chrono::steady_clock::now();
            long long ms = chrono::duration_cast<chrono::milliseconds>(deadline - now).count();
            long long left = ms > 0 ? (ms + 999) / 1000 : 0;
            if (left <= 0) {
                cout << "\n\n*** COUNTDOWN REACHED 00h 00m 00s ***\n";
                return "TIME_EXPIRED";
            }
            if (left != lastSec) {
                lastSec = left;
#ifdef _WIN32
                
                string title = "[TIME LEFT: " + Util::fmtLeft(left) + " | " + (isOnline ? "ONLINE" : "OFFLINE") + "] Online Exam System";
                SetConsoleTitleA(title.c_str());
#endif
                cout << "\r\033[K" << "[" << Util::fmtLeft(left) << " | " << (isOnline ? "ONLINE" : "OFFLINE") << "] " << prompt << input << flush;
            }

#ifdef _WIN32
            if (_kbhit()) {
                int ch = _getch();
                if (ch == '\r' || ch == '\n') {
                    cout << "\n";
                    return input;
                } else if (ch == '\b' || ch == 127) {
                    if (!input.empty()) {
                        input.pop_back();
                        cout << "\r\033[K" << "[" << Util::fmtLeft(left) << " | " << (isOnline ? "ONLINE" : "OFFLINE") << "] " << prompt << input << flush;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    input.push_back((char)ch);
                    cout << (char)ch << flush;
                }
            }
#else
            
            return ask(prompt);
#endif
            this_thread::sleep_for(chrono::milliseconds(50));
        }
    }
};
function<void()> Console::s_onEof;

class User {
    static int s_alive;
protected:
    string id_, name_;
    string sessionToken_;
public:
    explicit User(string id = "", string name = "") : id_(std::move(id)), name_(std::move(name)) { ++s_alive; }
    User(const User& o) : id_(o.id_), name_(o.name_), sessionToken_(o.sessionToken_) { ++s_alive; }
    User& operator=(const User& o) {
        if (this != &o) {
            id_ = o.id_;
            name_ = o.name_;
            sessionToken_ = o.sessionToken_;
        }
        return *this;
    }
    virtual ~User() { --s_alive; }
    virtual string role() const = 0;
    virtual void authenticate(AppContext& ctx) = 0;
    virtual void menu(AppContext& ctx) = 0;
    const string& id() const { return id_; }
    const string& name() const { return name_; }
    static int alive() { return s_alive; }
};
int User::s_alive = 0;

class PrivilegedUser : public User {
    string expectedRole_;
public:
    explicit PrivilegedUser(string expectedRole) : User(), expectedRole_(std::move(expectedRole)) {}
    void authenticate(AppContext&) override;
};


class Teacher : public PrivilegedUser {
public:
    Teacher() : PrivilegedUser("Teacher") {}
    string role() const override { return "Teacher"; }
    void menu(AppContext& ctx) override;
};


class Admin : public PrivilegedUser {
public:
    Admin() : PrivilegedUser("Admin") {}
    string role() const override { return "Admin"; }
    void menu(AppContext& ctx) override;
};

class Student : public User, public ISerializable {
    string roll_, pwHash_; int status_; long long start_; bool repaired_ = false;
public:
    enum Status { NotTaken = 0, InProgress = 1, Completed = 2 };
    Student() : Student("", "", "") {}
    Student(string id, string name, string roll = "")
        : User(std::move(id), std::move(name)), roll_(std::move(roll)), status_(NotTaken), start_(0) {}
    string role() const override { return "Student"; }
    void authenticate(AppContext& ctx) override;
    void menu(AppContext& ctx) override;
    const string& roll() const { return roll_; }
    int status() const { return status_; }
    long long start() const { return start_; }
    void begin(long long t) { status_ = InProgress; start_ = t; }
    void complete() { status_ = Completed; start_ = 0; }
    void resetExam() { status_ = NotTaken; start_ = 0; }
    bool operator==(const Student& o) const { return id_ == o.id_; }
    string toLine() const override { return id_ + "|" + name_ + "|" + roll_ + "|" + to_string(status_) + "|" + to_string(start_) + "|" + pwHash_; }
    bool fromLine(const string& l) override {
        auto p = Util::split(l); if (p.size() < 6 || !Util::validId(p[0])) return false;
        id_ = p[0]; name_ = p[1]; roll_ = p[2]; pwHash_ = p[5];
        long long st = Util::toLL(p[3], -1); status_ = inRange(st, 0LL, 2LL) ? (int)st : Completed;
        repaired_ = (status_ != st); start_ = Util::toLL(p[4], 0); return true;
    }
    string warning() const override { return repaired_ ? "bad status for " + id_ + "; set to BLOCKED." : ""; }
};

class Display {
public:
    static void show(const Result& r) { cout << r << "\n"; }
    static void show(const Question& q, int n, bool listView) {
        if (listView) {
            cout << right << setw(2) << n << ". [" << (q.isWritten() ? "WRITTEN" : "MCQ") << "] "
                 << q.text() << " [key: " << q.correct() << "]\n";
        } else {
            if (q.isWritten()) {
                cout << "\nQ" << n << " [Written / Short Answer]: " << q.text() << "\n";
                cout << "  (Write answer in plain text. Type 'CLEAR' to erase.)\n";
            } else {
                cout << "\nQ" << n << " [Multiple Choice]: " << q.text() << "\n";
                for (int i = 0; i < 4; i++) cout << "  " << char('A' + i) << ") " << q.option(i) << "\n";
                cout << "  (Choose A-D. Type 'CLEAR' to erase.)\n";
            }
        }
    }
    static void show(const Student& s, bool teacherView) {
        const char* st = s.status() == Student::NotTaken ? "not taken" : s.status() == Student::InProgress ? "in progress" : (teacherView ? "BLOCKED (completed)" : "completed");
        cout << s.id() << " | " << s.name() << " | " << s.roll() << " | " << st << "\n";
    }
};

class AppContext {
    static uint64_t makeKey() {
        string salt; { ifstream f("admin.dat"); getline(f, salt); }
        if (salt.empty()) { salt = Util::randomToken(16); Util::writeAtomic("admin.dat", salt + "\n"); }
        return Util::fnv(salt + "exam-secure-key");
    }
    XorProtector prot_;
    SecureStorage secure_;
    PlainStorage plain_;
public:
    Repository<Question> questions;
    Repository<Student> students;
    Repository<Result> results;
    AppContext() : prot_(makeKey()), secure_(prot_), questions(plain_, "questions.txt"), students(plain_, "students.txt"), results(secure_, "results") { seedDemo(); }
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
    SecureStorage& secure() { return secure_; }
    const IProtector& protector() const { return prot_; }
    int loadDuration() const { ifstream f("config.txt"); int m = 30; f >> m; return inRange(m, 1, 1440) ? m : 30; }
    bool saveDuration(int m) { return Util::writeAtomic("config.txt", to_string(m) + "\n"); }
private:
    void seedDemo() {
        if (!Util::fileExists("questions.txt")) {
            questions.add(Question("What is 12 + 8?", "18", "20", "22", "24", 'B'));
            questions.add(Question("What is the capital of India?", "Mumbai", "New Delhi", "Kolkata", "Chennai", 'B'));
            questions.add(Question("Which planet is called the Red Planet?", "Venus", "Jupiter", "Mars", "Saturn", 'C'));
            questions.add(Question("Explain Encapsulation in OOP briefly.", "Data hiding")); // Written question
            questions.save();
        }
        if (!Util::fileExists("students.txt")) {
            for (int i = 1; i <= 4; i++) students.add(Student("STU00" + to_string(i), "Demo Student " + to_string(i), "DEMO"));
            students.save();
        }
        if (!Util::fileExists("config.txt")) saveDuration(30);
    }
};

class ExamLock {
    string name_; atomic<bool> held_{false};
    static constexpr const char* BUSY = "This ID is already open in another exam window. Close it first.";
public:
    explicit ExamLock(const string& id) : name_("lock_" + id + ".txt") {
        if (Util::fileExists(name_)) {
            long long hb = 0; { ifstream f(name_); f >> hb; }
            if (hb == 0) { error_code ec; auto age = fs::file_time_type::clock::now() - fs::last_write_time(name_, ec); if (!ec && age < chrono::seconds(6)) throw LockException(BUSY); }
            else if (Util::nowSec() - hb < 6) throw LockException(BUSY);
            error_code ec; fs::remove(name_, ec);
        }
        FILE* fp = fopen(name_.c_str(), "wx"); if (!fp) throw LockException(BUSY);
        fprintf(fp, "%lld\n", Util::nowSec()); fclose(fp); held_ = true;
    }
    ~ExamLock() { release(); }
    void beat() { if (held_) Util::writeAtomic(name_, to_string(Util::nowSec()) + "\n"); }
    void release() { if (held_.exchange(false)) { error_code ec; fs::remove(name_, ec); } }
};

class ExamSession {
    AppContext& ctx_;
    Student& rec_;
    Student me_;
    vector<Question> qs_;
    AnswerSheet sheet_;
    mutex mx_; atomic<bool> done_{false};
    bool saved_ = true, online_ = true;
    chrono::steady_clock::time_point deadline_;
    unique_ptr<ExamLock> lock_;

    long long leftSec() const { auto ms = chrono::duration_cast<chrono::milliseconds>(deadline_ - chrono::steady_clock::now()).count(); return ms > 0 ? (ms + 999) / 1000 : 0; }

    void syncIfOnline(const string& body) {
        bool now = Network::isOnline();
        if (now != online_) {
            online_ = now;
            cout << "\n*** Connection " << (now ? "RESTORED (ONLINE: synced to server)" : "LOST (OFFLINE: local auto-save active)") << " ***\n";
        }
        if (online_) {
            string url = ServerConfig::getUrl() + "/api/sync_exam";
            string payload = "{\"student_id\": \"" + me_.id() + "\", \"data\": \"" + Util::toHex(body) + "\"}";
            HttpClient::post(url, payload);
        }
    }
    bool saveAnswers() {
        stringstream ss; ss << me_.id() << "|" << me_.name() << "|" << me_.roll() << "|" << me_.start() << "\n";
        for (size_t q = 1; q <= sheet_.size(); q++) {
            if (sheet_.answered(q)) {
                const Answer& a = sheet_.get(q);
                if (a.isWritten) ss << q << "|W|" << a.mode << "|" << a.t << "|" << a.text << "\n";
                else ss << q << "|" << a.choice << "|" << a.mode << "|" << a.t << "\n";
            }
        }
        bool ok = ctx_.secure().write("answers_" + me_.id(), ss.str());
        if (ok) syncIfOnline(ss.str());
        return ok;
    }
    void loadAnswers() {
        string base = "answers_" + me_.id(), t;
        if (!ctx_.secure().read(base, t)) return;
        stringstream ss(t); string l; getline(ss, l);
        while (getline(ss, l)) {
            auto p = Util::split(l); if (p.size() < 4) continue;
            int q = (int)Util::toLL(p[0], 0);
            if (q > 0 && q <= (int)sheet_.size()) {
                if (p[1] == "W" && p.size() >= 5) sheet_.setText((size_t)q, p[4], p[2], Util::toLL(p[3], 0));
                else if (!p[1].empty()) sheet_.setChoice((size_t)q, p[1][0], p[2], Util::toLL(p[3], 0));
            }
        }
    }
    Result grade() const {
        int score = 0;
        for (size_t i = 0; i < qs_.size(); i++) {
            if (!sheet_.answered(i + 1)) continue;
            const Answer& a = sheet_.get(i + 1);
            if (qs_[i].isWritten()) {
                // For written questions: evaluate match against sample keywords or exact phrase
                string cleanAns = Util::toLower(Util::trim(a.text));
                string cleanKey = Util::toLower(Util::trim(qs_[i].correct()));
                if (!cleanKey.empty() && cleanAns.find(cleanKey) != string::npos) score++;
            } else {
                if (!qs_[i].correct().empty() && a.choice == qs_[i].correct()[0]) score++;
            }
        }
        return Result(me_.id(), me_.name(), me_.roll(), score, (int)qs_.size(), Util::nowSec());
    }
    bool finish(bool autoEnd) {
        Result res = grade();
        bool ok = saveAnswers();
        try {
            try { ctx_.results.load(); }
            catch (const IntegrityException&) { ctx_.secure().quarantine("results"); ctx_.results.clear(); }
            ctx_.results.add(res);
            ok = ctx_.results.save() && ok;
        } catch (const ExamException& e) { cout << e.kind() << ": " << e.what() << "\n"; ok = false; }
        if (ok) { rec_.complete(); ok = ctx_.students.save(); }
        if (ok) {
            done_ = true;
            AuditLog::write("Exam finished: " + me_.id() + " score " + to_string(res.score()) + (autoEnd ? " (time up)" : ""));
            cout << "\n" << (autoEnd ? "Time up! Answers submitted. " : "Exam submitted successfully! ")
                 << "This ID is now locked. Answers cannot be cleared or changed.\n" << flush;
            return true;
        }
        cout << "\n*** SAVE FAILED. Tell the invigilator. ***\n" << flush;
        if (autoEnd) done_ = true;
        return false;
    }
    void onEof() {
        lock_guard<mutex> lk(mx_);
        if (!done_) { saveAnswers(); cout << "\nInput closed: progress saved, exam can be resumed.\n"; }
        lock_->release();
    }
    void watchdog() {
        long long lastWarn = -1, lastBeat = 0;
        while (!done_) {
            this_thread::sleep_for(chrono::milliseconds(200));
            long long left = leftSec(), now = Util::nowSec();
            if (now != lastBeat) { lastBeat = now; lock_->beat(); }
            if (left <= 0) {
                lock_guard<mutex> lk(mx_); if (done_) return;
                cout << "\n*** COUNTDOWN REACHED 00h 00m 00s ***\n";
                finish(true); cout << "Exiting.\n" << flush; lock_->release(); _Exit(0);
            }
            if ((left == 300 || left == 60 || left == 10) && left != lastWarn) {
                lastWarn = left; cout << "\n*** WARNING: " << Util::fmtLeft(left) << " left ***\n" << flush;
            }
        }
    }
    void showHeader() {
        int answered = sheet_.answeredCount(); vector<int> rem;
        for (size_t i = 1; i <= qs_.size(); i++) if (!sheet_.answered(i)) rem.push_back((int)i);
        cout << "\n======================================================\n"
             << " REAL-TIME COUNTDOWN: " << Util::fmtLeft(leftSec()) << "   | Mode: " << (Network::isOnline() ? "ONLINE (Connected)" : "OFFLINE (Local)") << "\n"
             << " Answered:  " << answered << "   Remaining: " << rem.size() << " of " << qs_.size() << "   " << (saved_ ? "[Saved]" : "[UNSAVED CHANGES]") << "\n";
        cout << " Question Status:\n  ";
        for (size_t i = 1; i <= qs_.size(); i++) cout << "Q" << i << sheet_.summary(i) << " ";
        cout << "\n Not answered yet:";
        if (rem.empty()) cout << " None (All answered)";
        for (int r : rem) cout << " Q" << r;
        cout << "\n======================================================\n";
    }
public:
    ExamSession(AppContext& ctx, Student& rec) : ctx_(ctx), rec_(rec) {}
    void run() {
        if (rec_.status() == Student::Completed) { cout << "You have already submitted the exam. Answers cannot be modified.\n"; return; }
        ctx_.questions.load(); if (ctx_.questions.empty()) { cout << "No questions available. Contact teacher.\n"; return; }
        qs_ = ctx_.questions.items();
        try { lock_.reset(new ExamLock(rec_.id())); }
        catch (const LockException& e) { cout << e.what() << "\n"; return; }
        me_ = rec_; sheet_ = AnswerSheet(qs_.size());
        if (rec_.status() == Student::NotTaken) {
            rec_.begin(Util::nowSec());
            if (!ctx_.students.save()) { cout << "ERROR: cannot write students file.\n"; return; }
            me_ = rec_;
        }
        loadAnswers();
        online_ = Network::isOnline(); saved_ = true; done_ = false;
        long long remain = me_.start() + (long long)ctx_.loadDuration() * 60 - Util::nowSec();
        deadline_ = chrono::steady_clock::now() + chrono::seconds(remain > 0 ? remain : 0);
        AuditLog::write("Exam started/resumed: " + me_.id());
        if (remain <= 0) { cout << "\nYour exam time has ended.\n"; lock_guard<mutex> lk(mx_); finish(true); return; }

        cout << "\nExam in progress. Real-time timer active. Left: " << Util::fmtLeft(leftSec()) << "\n"
             << "Commands:\n"
             << "  <number>        : Open question to answer (e.g. '1')\n"
             << "  <number> <ans>  : Quick-answer question (e.g. '1 C', '3 B')\n"
             << "  CLR             : CLEAR answer for a question\n"
             << "  V               : Save answers\n"
             << "  N               : TOGGLE NETWORK (Simulate Online/Offline switch mid-exam)\n"
             << "  X               : Exit & Submit exam\n";

        Console::setEofHook([this] { onEof(); });
        thread wd(&ExamSession::watchdog, this);

        while (true) {
            { lock_guard<mutex> lk(mx_); if (done_) break; showHeader(); }
            string c = Util::trim(Console::askLive("Command (Q-no / CLR=clear / V=save / N=net switch / X=submit) > ", deadline_, Network::isOnline()));
            if (c == "TIME_EXPIRED") {
                lock_guard<mutex> lk(mx_);
                finish(true);
                break;
            }

            if (c == "N" || c == "n" || c == "NET" || c == "net") {
                Network::toggleSimulation();
                bool nowOnline = Network::isOnline();
                cout << "\n*** [MODE SWITCH] Network mode changed to: " 
                     << (nowOnline ? "ONLINE (Reconnected)" : "OFFLINE (Disconnected)") << " ***\n";
                if (nowOnline) {
                    cout << "Auto-syncing all accumulated answers to server... ";
                    if (saveAnswers()) cout << "SUCCESS!\n";
                    else cout << "FAILED!\n";
                } else {
                    cout << "Offline mode active: answers are saved securely to local disk.\n";
                }
                continue;
            }

            string upperC = c;
            for (char &ch : upperC) ch = (char)toupper((unsigned char)ch);

            if (upperC == "CLR" || upperC == "CLEAR" || upperC.rfind("CLR ", 0) == 0 || upperC.rfind("CLEAR ", 0) == 0) {
                string qStr;
                if (upperC.rfind("CLR ", 0) == 0) qStr = Util::trim(c.substr(4));
                else if (upperC.rfind("CLEAR ", 0) == 0) qStr = Util::trim(c.substr(6));
                else qStr = Util::trim(Console::ask("Enter question number to CLEAR: "));

                int qn = 0;
                if (Util::parseUInt(qStr, qn, (int)qs_.size()) && qn >= 1) {
                    lock_guard<mutex> lk(mx_);
                    if (sheet_.answered((size_t)qn)) {
                        sheet_.clear((size_t)qn);
                        saved_ = false;
                        cout << "-> Answer for Question " << qn << " has been CLEARED.\n";
                        saveAnswers();
                    } else {
                        cout << "Question " << qn << " has no answer to clear.\n";
                    }
                } else {
                    cout << "Invalid question number.\n";
                }
                continue;
            }

            if (upperC == "A" || upperC == "B" || upperC == "C" || upperC == "D") {
                cout << "-> Option '" << upperC << "' entered. To answer a question, enter '<question_no> " << upperC << "' (e.g. '1 " << upperC << "') or open question number first.\n";
                cout << "   (Note: To clear an answer, use the 'CLR' command).\n";
                continue;
            }

            if (c == "V" || c == "v") {
                lock_guard<mutex> lk(mx_); if (done_) break;
                if (saveAnswers()) { saved_ = true; cout << "Answers saved successfully.\n"; }
                else cout << "*** SAVE FAILED ***\n";
                continue;
            }

            if (c == "X" || c == "x") {
                { lock_guard<mutex> lk(mx_); if (done_) break; if (!saved_) { cout << "Please SAVE (V) before submitting.\n"; continue; } }
                if (Console::ask("Submit exam and finalize? Once submitted, answers CANNOT be changed or cleared (y/n): ") != "y") continue;
                lock_guard<mutex> lk(mx_); if (done_) break;
                if (!saved_) { cout << "Changes pending. Save first (V).\n"; continue; }
                if (finish(false)) break;
                continue;
            }

            int quickQn = 0;
            char quickChoice = 0;
            size_t splitPos = c.find_first_of(" :=");
            if (splitPos != string::npos) {
                string numPart = Util::trim(c.substr(0, splitPos));
                string ansPart = Util::trim(c.substr(splitPos + 1));
                if (Util::parseUInt(numPart, quickQn, (int)qs_.size()) && quickQn >= 1) {
                    if (ansPart.size() == 1) {
                        char ch = (char)toupper((unsigned char)ansPart[0]);
                        if (ch >= 'A' && ch <= 'D') quickChoice = ch;
                    }
                }
            } else if (c.size() >= 2) {
                char lastCh = (char)toupper((unsigned char)c.back());
                if (lastCh >= 'A' && lastCh <= 'D') {
                    string numPart = c.substr(0, c.size() - 1);
                    if (Util::parseUInt(numPart, quickQn, (int)qs_.size()) && quickQn >= 1) {
                        quickChoice = lastCh;
                    }
                }
            }

            if (quickQn >= 1 && quickChoice != 0) {
                const Question& curQ = qs_[quickQn - 1];
                if (curQ.isWritten()) {
                    cout << "Question " << quickQn << " is a Written question. Please enter question number to type your answer.\n";
                } else {
                    string mode = Network::isOnline() ? "ON" : "OFF";
                    lock_guard<mutex> lk(mx_);
                    sheet_.setChoice((size_t)quickQn, quickChoice, mode, Util::nowSec());
                    cout << "Choice " << quickChoice << " recorded for Q" << quickQn << " (Mode: " << mode << ").\n";
                    saved_ = false;
                    saveAnswers();
                }
                continue;
            }

            int n = 0; if (!Util::parseUInt(c, n, (int)qs_.size()) || n < 1) { cout << "Invalid command or question number.\n"; continue; }
            const Question& curQ = qs_[n - 1];
            Display::show(curQ, n, false);

            if (sheet_.answered((size_t)n)) {
                const Answer& existing = sheet_.get((size_t)n);
                if (existing.isWritten) cout << "Current saved answer: \"" << existing.text << "\"\n";
                else cout << "Current choice: [" << existing.choice << "]\n";
                cout << "(Type new answer, or type 'CLEAR' to erase)\n";
            }

            string mode = Network::isOnline() ? "ON" : "OFF";
            if (curQ.isWritten()) {
                string ans = Console::ask("Enter your written answer (or type 'CLEAR' to erase): ");
                string tr = Util::trim(ans);
                lock_guard<mutex> lk(mx_); if (done_) break;
                string upTr = tr;
                for (char &x : upTr) x = (char)toupper((unsigned char)x);
                if (upTr == "CLEAR" || upTr == "CLR") {
                    sheet_.clear((size_t)n);
                    cout << "Answer for Q" << n << " cleared.\n";
                } else if (!tr.empty()) {
                    sheet_.setText((size_t)n, Util::clean(ans), mode, Util::nowSec());
                    cout << "Written answer recorded for Q" << n << " (Mode: " << mode << ").\n";
                }
            } else {
                string a = Util::trim(Console::ask("Choice (A-D, or type 'CLEAR' to erase): "));
                lock_guard<mutex> lk(mx_); if (done_) break;
                string upA = a;
                for (char &x : upA) x = (char)toupper((unsigned char)x);
                if (upA == "CLEAR" || upA == "CLR") {
                    sheet_.clear((size_t)n);
                    cout << "Choice for Q" << n << " cleared.\n";
                } else if (a.size() == 1 && inRange((char)toupper((unsigned char)a[0]), 'A', 'D')) {
                    char choiceChar = (char)toupper((unsigned char)a[0]);
                    sheet_.setChoice((size_t)n, choiceChar, mode, Util::nowSec());
                    cout << "Choice " << choiceChar << " recorded for Q" << n << " (Mode: " << mode << ").\n";
                } else {
                    cout << "Invalid choice. Please enter A-D or type 'CLEAR'.\n";
                    continue;
                }
            }
            saved_ = false;
            saveAnswers();
        }
        done_ = true; wd.join(); Console::setEofHook(nullptr);
    }
};

void PrivilegedUser::authenticate(AppContext&) {
    string id = Util::trim(Console::ask(role() + " ID: "));
    if (id.empty()) throw AuthException("ID cannot be empty.");
    string pw = Console::ask("Password: ");
    if (pw.empty()) throw AuthException("Password cannot be empty.");

    if (Network::isOnline()) {
        string url = ServerConfig::getUrl() + "/api/login";
        string payload = "{\"id\": \"" + id + "\", \"password\": \"" + pw + "\"}";
        HttpResponse res = HttpClient::post(url, payload);
        if (res.ok && HttpClient::extractJsonBool(res.body, "success")) {
            string serverRole = HttpClient::extractJson(res.body, "role");
            if (serverRole != expectedRole_) throw AuthException("Access denied: role mismatch.");
            id_ = id;
            name_ = HttpClient::extractJson(res.body, "name");
            sessionToken_ = HttpClient::extractJson(res.body, "token");
            cout << "Connected to central server. Welcome " << name_ << " (" << serverRole << ")!\n";
            return;
        } else {
            string err = HttpClient::extractJson(res.body, "error");
            throw AuthException(err.empty() ? "Server rejected credentials." : err);
        }
    } else {
        cout << "[OFFLINE MODE] Server offline. Using emergency local credentials.\n";
        if (expectedRole_ == "Teacher" && (id == "890teach" || id == "890mik") && (pw == "teacher123" || pw == "admin123")) { id_ = id; name_ = "Teacher"; return; }
        if (expectedRole_ == "Admin" && (id == "admin_sys" || id == "Mikhael@123") && (pw == "admin123" || pw == "master123")) { id_ = id; name_ = "Admin"; return; }
        throw AuthException("Offline verification failed.");
    }
}

void Student::authenticate(AppContext& ctx) {
    string id = Util::trim(Console::ask("Student ID: "));
    if (id.empty()) throw AuthException("");
    string pw = Console::ask("Password: ");
    if (pw.empty()) throw AuthException("Password cannot be empty.");

    if (Network::isOnline()) {
        string url = ServerConfig::getUrl() + "/api/login";
        string payload = "{\"id\": \"" + id + "\", \"password\": \"" + pw + "\"}";
        HttpResponse res = HttpClient::post(url, payload);
        if (res.ok && HttpClient::extractJsonBool(res.body, "success")) {
            id_ = id;
            name_ = HttpClient::extractJson(res.body, "name");
            roll_ = HttpClient::extractJson(res.body, "roll");
            sessionToken_ = HttpClient::extractJson(res.body, "token");
            cout << "Server login successful! Welcome, " << name_ << " (" << roll_ << ")\n";

            // Sync with local student cache
            ctx.students.load();
            Student* rec = ctx.students.find([&](const Student& x) { return x.id() == id; });
            if (!rec) { ctx.students.add(*this); ctx.students.save(); }
            else { *this = *rec; }
            return;
        } else {
            string err = HttpClient::extractJson(res.body, "error");
            throw AuthException(err.empty() ? "Server login failed." : err);
        }
    } else {
        cout << "[OFFLINE MODE] Central server not reached. Validating with local cache...\n";
        ctx.students.load();
        Student* rec = ctx.students.find([&](const Student& x) { return x.id() == id; });
        if (!rec) throw AuthException("Student ID not found in local records. Connect to server first.");
        *this = *rec;
    }
}

void Student::menu(AppContext& ctx) {
    while (true) {
        cout << "\n--- STUDENT: " << name_ << " ---\n1 Take exam\n2 View my result\n0 Logout\n";
        string c = Util::trim(Console::ask("> "));
        if (c == "1") {
            ctx.students.load();
            Student* rec = ctx.students.find([&](const Student& x) { return x.id() == id_; });
            if (!rec) { cout << "Student record error.\n"; continue; }
            ExamSession session(ctx, *rec);
            session.run();
        } else if (c == "2") {
            try {
                ctx.results.load(); const Result* last = nullptr;
                for (const Result& r : ctx.results) if (r.id() == id_) last = &r;
                if (last) Display::show(*last); else cout << "No result available for your ID.\n";
            } catch (const IntegrityException&) { cout << "Results file damaged or unreadable.\n"; }
        } else if (c == "0") return;
    }
}

void Teacher::menu(AppContext& ctx) {
    while (true) {
        ctx.questions.load();
        cout << "\n--- TEACHER ---\n1 Add question (MCQ or Written)\n2 Delete question\n3 Update question\n4 List questions\n5 Set exam duration\n6 List students\n7 Allow re-exam (remove block)\n0 Logout\n";
        string c = Util::trim(Console::ask("> "));
        try {
            if (c == "1" || c == "3") {
                int n = -1;
                if (c == "3") {
                    if (!Util::parseUInt(Console::ask("Question no: "), n, (int)ctx.questions.size()) || n < 1) throw ValidationException("Invalid.");
                    n--;
                }
                string qType = Util::trim(Console::ask("Type (1: Multiple Choice, 2: Written/Subjective): "));
                string qText = Util::clean(Console::ask("Question text: "));
                Question q;
                q.setText(qText);
                if (qType == "2") {
                    q.setType('W');
                    q.setCorrect(Util::clean(Console::ask("Expected keyword/key answer: ")));
                } else {
                    q.setType('M');
                    for (int i = 0; i < 4; i++) q.setOption(i, Util::clean(Console::ask(string("Option ") + char('A' + i) + ": ")));
                    q.setCorrect(Util::trim(Console::ask("Correct choice (A-D): ")));
                }
                if (n < 0) ctx.questions.add(q); else ctx.questions.at((size_t)n) = q;
                cout << (ctx.questions.save() ? "Saved successfully.\n" : "ERROR saving questions.\n");
                AuditLog::write("Teacher " + string(n < 0 ? "added" : "updated") + " question");
            } else if (c == "2") {
                int n = 0; if (!Util::parseUInt(Console::ask("Question no: "), n, (int)ctx.questions.size()) || n < 1) throw ValidationException("Invalid.");
                ctx.questions.removeAt((size_t)(n - 1)); cout << (ctx.questions.save() ? "Deleted.\n" : "ERROR.\n");
                AuditLog::write("Teacher deleted question " + to_string(n));
            } else if (c == "4") {
                int i = 1; for (const Question& q : ctx.questions) Display::show(q, i++, true);
            } else if (c == "5") {
                int m = 0;
                if (Util::parseUInt(Console::ask("Duration in minutes (1-1440): "), m, 1440) && m > 0) {
                    cout << (ctx.saveDuration(m) ? "Set to " + Util::fmtLeft(m * 60LL) + ".\n" : "ERROR.\n");
                    AuditLog::write("Teacher set duration " + to_string(m) + " min");
                } else throw ValidationException("Invalid.");
            } else if (c == "6") {
                ctx.students.load(); for (const Student& s : ctx.students) Display::show(s, true);
            } else if (c == "7") {
                ctx.students.load(); string id = Util::trim(Console::ask("Student ID: "));
                Student* s = ctx.students.find([&](const Student& x) { return x.id() == id && x.status() != Student::NotTaken; });
                if (!s) { cout << "No blocked student found with that ID.\n"; continue; }
                s->resetExam();
                if (!ctx.students.save()) throw FileException("could not write students.txt");
                cout << "Block removed. Student can take exam again.\n";
                AuditLog::write("Teacher allowed re-exam for " + id);
            } else if (c == "0") return;
        }
        catch (const ValidationException& e) { cout << e.what() << "\n"; }
        catch (const ExamException& e) { cout << e.kind() << ": " << e.what() << "\n"; }
    }
}

void Admin::menu(AppContext& ctx) {
    while (true) {
        cout << "\n--- ADMIN (Superuser) ---\n1 Examination results (all)\n2 View student answers\n3 Register student on server\n4 List students\n5 View activity log (last 1 KB)\n6 Server status & database\n0 Logout\n";
        string c = Util::trim(Console::ask("> "));
        try {
            if (c == "1") {
                try { ctx.results.load(); }
                catch (const IntegrityException&) { cout << "WARNING: Results file modified or damaged.\n"; continue; }
                if (ctx.results.empty()) cout << "No results yet.\n";
                for (const Result& r : ctx.results) Display::show(r);
            } else if (c == "2") {
                string t, id = Util::trim(Console::ask("Student ID: "));
                if (Util::validId(id) && ctx.secure().read("answers_" + id, t)) cout << t;
                else cout << "No readable answers file.\n";
            } else if (c == "3") {
                string id = Util::trim(Console::ask("New student ID: "));
                if (!Util::validId(id)) throw ValidationException("Invalid ID.");
                string nm = Util::clean(Console::ask("Name: "));
                string rl = Util::clean(Console::ask("Roll no: "));
                string pw = Console::ask("Password for student: ");

                if (Network::isOnline()) {
                    string url = ServerConfig::getUrl() + "/api/add_user";
                    string payload = "{\"id\": \"" + id + "\", \"name\": \"" + nm + "\", \"roll\": \"" + rl + "\", \"role\": \"Student\", \"password\": \"" + pw + "\"}";
                    HttpResponse res = HttpClient::post(url, payload);
                    if (res.ok) cout << "Registered on central server!\n";
                    else cout << "Server error: " << HttpClient::extractJson(res.body, "error") << "\n";
                }
                ctx.students.load();
                ctx.students.add(Student(id, nm, rl));
                ctx.students.save();
                cout << "Student saved locally.\n";
                AuditLog::write("Admin added student " + id);
            } else if (c == "4") {
                ctx.students.load(); for (const Student& s : ctx.students) Display::show(s, false);
            } else if (c == "5") {
                cout << AuditLog::tail(1024);
            } else if (c == "6") {
                cout << "Server URL:    " << ServerConfig::getUrl() << "\n";
                cout << "Server Status: " << (Network::isOnline() ? "ONLINE" : "OFFLINE") << "\n";
            } else if (c == "0") return;
        }
        catch (const ValidationException& e) { cout << e.what() << "\n"; }
        catch (const ExamException& e) { cout << e.kind() << ": " << e.what() << "\n"; }
    }
}

void enter(User& u, AppContext& ctx) {
    try { u.authenticate(ctx); }
    catch (const AuthException& e) {
        if (*e.what()) cout << e.what() << "\n";
        AuditLog::write("Login failed (" + u.role() + ")");
        return;
    }
    AuditLog::write(u.role() + " logged in: " + u.id());
    u.menu(ctx);
    AuditLog::write(u.role() + " logged out: " + u.id());
}

int main() {
    set_terminate([] { cerr << "\nFATAL: unexpected error. Exiting.\n"; abort(); });
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &dwMode)) {
        SetConsoleMode(hOut, dwMode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    }
#endif
    try {
        AppContext ctx;
        cout << "System initialized. Connecting to server (" << ServerConfig::getUrl() << ")... ";
        if (Network::isOnline()) cout << "[ONLINE]\n";
        else cout << "[OFFLINE - local mode]\n";

        while (true) {
            cout << "\n===== ONLINE EXAMINATION SYSTEM (v5) =====\n"
                 << "1 Student login\n2 Teacher login\n3 Admin login\n0 Exit\n";
            string c = Util::trim(Console::ask("> "));
            if (c == "0") break;
            unique_ptr<User> u;
            if (c == "1") u = make_unique<Student>();
            else if (c == "2") u = make_unique<Teacher>();
            else if (c == "3") u = make_unique<Admin>();
            else continue;
            enter(*u, ctx);
        }
    }
    catch (const ExamException& e) { cerr << e.kind() << ": " << e.what() << "\n"; return 1; }
    catch (const exception& e) { cerr << "Unexpected error: " << e.what() << "\n"; return 1; }
    return 0;
}
