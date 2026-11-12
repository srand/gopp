// mandelbrot: render an ASCII fractal with a small goroutine worker pool.
// Rows are calculated concurrently and printed in order when workers finish.
//
//     $ mandelbrot
#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace gopp;

constexpr char palette[] = " .:-=+*#%@";

struct Row {
    int y;
    std::string pixels;
};

int main() {
    constexpr int width = 100;
    constexpr int height = 40;
    constexpr int max_iterations = 500;

    auto jobs = make_chan<int>(height);
    auto rows = make_chan<Row>(height);
    sync::WaitGroup workers;
    for (int i = 0; i < 8; ++i) {
        workers.Go([jobs, rows] {
            for (int y : jobs) {
                std::string pixels;
                pixels.reserve(width);
                double cy = 1.2 - 2.4 * y / (height - 1);
                for (int x = 0; x < width; ++x) {
                    double cx = -2.1 + 3.0 * x / (width - 1);
                    double zx = 0.0, zy = 0.0;
                    int iteration = 0;
                    while (zx * zx + zy * zy <= 4.0 && iteration < max_iterations) {
                        double next_zx = zx * zx - zy * zy + cx;
                        zy = 2.0 * zx * zy + cy;
                        zx = next_zx;
                        ++iteration;
                    }
                    std::size_t shade = iteration == max_iterations
                        ? sizeof(palette) - 2
                        : static_cast<std::size_t>(iteration) * (sizeof(palette) - 2) / max_iterations;
                    pixels.push_back(palette[shade]);
                }
                rows.send(Row{y, std::move(pixels)});
            }
        });
    }

    for (int y = 0; y < height; ++y) jobs.send(y);
    jobs.close();
    workers.Wait();
    rows.close();

    std::vector<std::string> image(height);
    for (Row row : rows) image[row.y] = std::move(row.pixels);
    for (const auto& line : image) fmt::Println("{}", line);
}
