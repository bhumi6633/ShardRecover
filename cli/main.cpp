#include <iostream>
#include <string_view>

namespace {

void print_help()
{
    std::cout << "Usage: shardrecover [--help] [--version]\n"
                 "\n"
                 "Planned commands:\n"
                 "  inspect\n"
                 "  fragment\n"
                 "  analyze\n"
                 "  reconstruct\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc == 1) {
        print_help();
        return 0;
    }

    const std::string_view argument{argv[1]};

    if (argc == 2 && argument == "--help") {
        print_help();
        return 0;
    }

    if (argc == 2 && argument == "--version") {
        std::cout << "ShardRecover 0.1.0\n";
        return 0;
    }

    std::cerr << "Error: unknown argument '" << argument << "'\n";
    return 1;
}
