#include <metal/metal.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CliOptions {
    std::string database;
    std::string output{"-"};
    metal::EntityGeneratorOptions generator;
    metal::IntrospectOptions introspection;
};

std::vector<std::string> split_csv(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto end = comma == std::string_view::npos ? value.size() : comma;
        if (end > start) result.emplace_back(value.substr(start, end - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

void usage(std::ostream& out) {
    out <<
        "metal-orm-gen " METAL_ORM_VERSION "\n"
        "Generate C++26 MetalORM models from an SQLite database.\n\n"
        "Usage:\n"
        "  metal-orm-gen --db=app.sqlite [--out=entities.hpp] [options]\n\n"
        "Options:\n"
        "  --db=PATH             SQLite database path (required)\n"
        "  --out=PATH            Output header; '-' writes to stdout (default)\n"
        "  --namespace=NAME      Generated namespace (default: entities)\n"
        "  --include=a,b         Only introspect named tables\n"
        "  --exclude=a,b         Exclude named tables\n"
        "  --include-views       Generate introspected views as read-only mapped models\n"
        "  --no-relations        Do not emit belongs_to relation wrappers from foreign keys\n"
        "  --no-comments         Do not emit schema comments as /// documentation\n"
        "  --version             Print version\n"
        "  --help                Print this help\n";
}

CliOptions parse(int argc, char** argv) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            std::exit(0);
        }
        if (arg == "--version") {
            std::cout << "metal-orm-gen " METAL_ORM_VERSION << '\n';
            std::exit(0);
        }
        if (arg == "--include-views") {
            options.introspection.include_views = true;
            continue;
        }
        if (arg == "--no-relations") {
            options.generator.emit_relations = false;
            continue;
        }
        if (arg == "--no-comments") {
            options.generator.emit_comments = false;
            continue;
        }

        const auto equal = arg.find('=');
        if (equal == std::string_view::npos) {
            throw std::invalid_argument("MetalORM: unknown generator argument: " + std::string(arg));
        }
        const auto key = arg.substr(0, equal);
        const auto value = arg.substr(equal + 1);
        if (key == "--db") options.database = std::string(value);
        else if (key == "--out") options.output = std::string(value);
        else if (key == "--namespace") options.generator.namespace_name = std::string(value);
        else if (key == "--include") options.introspection.include_tables = split_csv(value);
        else if (key == "--exclude") options.introspection.exclude_tables = split_csv(value);
        else throw std::invalid_argument("MetalORM: unknown generator argument: " + std::string(key));
    }

    if (options.database.empty()) {
        throw std::invalid_argument("MetalORM: --db=PATH is required");
    }
    return options;
}

void write_output(const std::string& path, const std::string& code) {
    if (path == "-") {
        std::cout << code;
        return;
    }
    const std::filesystem::path output{path};
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("MetalORM: cannot open generator output: " + path);
    stream << code;
    if (!stream) throw std::runtime_error("MetalORM: failed writing generator output: " + path);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        metal::SQLiteExecutor executor{options.database};
        const auto generated = metal::generate_sqlite_model_header(
            executor,
            options.generator,
            options.introspection);
        write_output(options.output, generated.code);
        for (const auto& warning : generated.warnings) {
            std::cerr << "metal-orm-gen: warning: " << warning << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "metal-orm-gen: " << error.what() << '\n';
        return 1;
    }
}
