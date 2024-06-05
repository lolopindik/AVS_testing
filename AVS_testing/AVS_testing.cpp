#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <yaml-cpp/yaml.h>
#include <boost/asio.hpp>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>

#define _CRT_SECURE_NO_WARNINGS

using namespace std;

// Структура для представления действия с файлом
struct FileAction {
    string name;
    string file;
    vector<string> actions;
    vector<string> dependencies;
};

// Структура для представления конфигурации
struct Config {
    vector<FileAction> files;
    string host;
    string endpoint;
};

// Функция для парсинга конфигурации из YAML файла
Config parseConfig(const string& filename) {
    YAML::Node config = YAML::LoadFile(filename);
    Config cfg;
    for (const auto& fileNode : config["files"]) {
        FileAction fileAction;
        fileAction.name = fileNode["name"].as<string>();
        fileAction.file = fileNode["file"].as<string>();
        for (const auto& action : fileNode["actions"]) {
            fileAction.actions.push_back(action.as<string>());
        }
        if (fileNode["dependencies"]) {
            for (const auto& dependency : fileNode["dependencies"]) {
                fileAction.dependencies.push_back(dependency.as<string>());
            }
        }
        cfg.files.push_back(fileAction);
    }
    cfg.host = config["host"].as<string>();
    cfg.endpoint = config["endpoint"].as<string>();
    return cfg;
}

// Класс для загрузки файлов
class Downloader {
public:
    Downloader(boost::asio::io_context& io_context)
        : strand_(boost::asio::make_strand(io_context)) {}

    void download(const string& url, const string& file, function<void(bool)> callback) {
        boost::asio::post(strand_, [this, url, file, callback]() { this->do_download(url, file, callback); });
    }

private:
    void do_download(const string& url, const string& file, function<void(bool)> callback) {
        CURL* curl;
        FILE* fp;
        CURLcode res;
        curl = curl_easy_init();
        if (curl) {
            fopen_s(&fp, file.c_str(), "wb");
            if (!fp) {
                cerr << "Ошибка открытия файла: " << file << endl;
                callback(false);
                return;
            }
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            fclose(fp);
            if (res != CURLE_OK) {
                cerr << "Сбой загрузки: " << curl_easy_strerror(res) << endl;
                callback(false);
            }
            else {
                callback(true);
            }
        }
        else {
            callback(false);
        }
    }

    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
};

// Вспомогательная функция для копирования данных
int copyData(struct archive* ar, struct archive* aw) {
    const void* buff;
    size_t size;
    la_int64_t offset;
    int r;

    for (;;) {
        la_ssize_t read_result = archive_read_data_block(ar, &buff, &size, &offset);
        if (read_result == ARCHIVE_EOF)
            return (ARCHIVE_OK);
        if (read_result < ARCHIVE_OK)
            return (static_cast<int>(read_result));
        int write_result = archive_write_data_block(aw, buff, size, offset);
        if (write_result < ARCHIVE_OK) {
            cerr << archive_error_string(aw) << endl;
            return (static_cast<int>(write_result));
        }
    }
}

// Функция для распаковки архива
bool unpack(const string& filename, const string& outdir) {
    struct archive* a;
    struct archive* ext;
    struct archive_entry* entry;
    int flags;
    int r;

    a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_compression_all(a);
    ext = archive_write_disk_new();
    flags = ARCHIVE_EXTRACT_TIME;
    flags |= ARCHIVE_EXTRACT_PERM;
    flags |= ARCHIVE_EXTRACT_ACL;
    flags |= ARCHIVE_EXTRACT_FFLAGS;
    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);
    if ((r = archive_read_open_filename(a, filename.c_str(), 10240))) {
        cerr << "Не удалось открыть файл: " << filename << endl;
        return false;
    }
    // Бесконечный цикл без условий
    for (;;) {
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_OK)
            cerr << archive_error_string(a) << endl;
        if (r < ARCHIVE_WARN)
            return false;
        const char* currentFile = archive_entry_pathname(entry);
        const string fullOutputPath = outdir + "/" + currentFile;
        archive_entry_set_pathname(entry, fullOutputPath.c_str());
        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK)
            cerr << archive_error_string(ext) << endl;
        else if (archive_entry_size(entry) > 0) {
            copyData(a, ext);
            if (r < ARCHIVE_OK)
                cerr << archive_error_string(ext) << endl;
            if (r < ARCHIVE_WARN)
                return false;
        }
        r = archive_write_finish_entry(ext);
        if (r < ARCHIVE_OK)
            cerr << archive_error_string(ext) << endl;
        if (r < ARCHIVE_WARN)
            return false;
    }
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return true;
}

// Основная функция
int main(int argc, char* argv[]) {

    setlocale(LC_ALL, "rus");

    if (argc < 2) {
        cerr << "Использование: " << argv[0] << " <config_file>" << endl;
        return 1;
    }

    Config cfg = parseConfig(argv[1]);
    boost::asio::io_context io_context;
    Downloader downloader(io_context);

    for (const auto& file : cfg.files) {
        downloader.download(file.file, file.name, [&file](bool success) {
            if (success) {
                cout << "Загружено: " << file.name << endl;
                if (!file.actions.empty() && file.actions[0] == "unpack") {
                    if (unpack(file.name, "output_directory")) {
                        cout << "Распаковано: " << file.name << endl;
                    }
                    else {
                        cerr << "Не удалось распаковать: " << file.name << endl;
                    }
                }
            }
            else {
                cerr << "Не удалось загрузить: " << file.name << endl;
            }
            });
    }

    io_context.run();

    return 0;
}