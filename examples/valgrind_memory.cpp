#include <iostream>
#include <memory>
#include <string>

int main(int argc, char* argv[]) {
    const bool broken = argc > 1 && std::string(argv[1]) == "bug";
    if (broken) {
        // Error intencionado: usamos memoria despues de liberarla.
        int* value = new int(42);
        delete value;
        std::cout << *value << '\n';
    } else {
        // RAII mantiene el objeto vivo hasta que termina este bloque.
        auto value = std::make_unique<int>(42);
        std::cout << *value << '\n';
    }
}
