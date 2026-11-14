// io.MultiReader, io.MultiWriter and io.TeeReader, ported from Go 1.27.3's
// io/multi.go and io/io.go.
#include <gopp/io.hpp>

#include <deque>
#include <utility>

namespace gopp::io {

namespace {

class multiReader final : public Reader, public WriterTo {
public:
    explicit multiReader(std::vector<ptr<Reader>> readers) : readers_(readers.begin(), readers.end()) {}

    std::pair<isize, error> Read(std::span<byte> p) override {
        while (!readers_.empty()) {
            // Flatten nested multiReaders (Go's issue 13558).
            if (readers_.size() == 1) {
                if (auto* r = dynamic_cast<multiReader*>(readers_.front().get())) {
                    auto inner = r->readers_;  // a copy: r may still be read on its own
                    readers_ = std::move(inner);
                    continue;
                }
            }
            auto [n, err] = readers_.front()->Read(p);
            if (err == EOF_) readers_.pop_front();
            if (n > 0 || err != EOF_) {
                if (err == EOF_ && !readers_.empty()) err = nullptr;  // more readers remain
                return {n, err};
            }
        }
        return {0, EOF_};
    }

    std::pair<int64, error> WriteTo(Writer& w) override {
        int64 sum = 0;
        while (!readers_.empty()) {
            if (auto* mr = dynamic_cast<multiReader*>(readers_.front().get())) {
                auto [n, err] = mr->WriteTo(w);
                sum += n;
                if (err) return {sum, err};
            } else {
                auto [n, err] = Copy(w, *readers_.front());
                sum += n;
                if (err) {
                    // Keep this reader, for a later Read.
                    return {sum, err};
                }
            }
            readers_.pop_front();
        }
        return {sum, nullptr};
    }

private:
    std::deque<ptr<Reader>> readers_;
};

class multiWriter final : public Writer {
public:
    explicit multiWriter(std::vector<ptr<Writer>> writers) {
        for (auto& w : writers) {
            if (auto* mw = dynamic_cast<multiWriter*>(w.get())) {
                writers_.insert(writers_.end(), mw->writers_.begin(), mw->writers_.end());
            } else {
                writers_.push_back(std::move(w));
            }
        }
    }

    std::pair<isize, error> Write(std::span<const byte> p) override {
        for (auto& w : writers_) {
            auto [n, err] = w->Write(p);
            if (err) return {n, err};
            if (n != isize(p.size())) return {n, ErrShortWrite};
        }
        return {isize(p.size()), nullptr};
    }

private:
    std::vector<ptr<Writer>> writers_;
};

class teeReader final : public Reader {
public:
    teeReader(ptr<Reader> r, ptr<Writer> w) : r_(std::move(r)), w_(std::move(w)) {}

    std::pair<isize, error> Read(std::span<byte> p) override {
        auto [n, err] = r_->Read(p);
        if (n > 0) {
            if (auto [nw, werr] = w_->Write(p.first(std::size_t(n))); werr) return {nw, werr};
        }
        return {n, err};
    }

private:
    ptr<Reader> r_;
    ptr<Writer> w_;
};

}  // namespace

ptr<Reader> MultiReader(std::vector<ptr<Reader>> readers) { return make_ptr<multiReader>(std::move(readers)); }
ptr<Writer> MultiWriter(std::vector<ptr<Writer>> writers) { return make_ptr<multiWriter>(std::move(writers)); }
ptr<Reader> TeeReader(ptr<Reader> r, ptr<Writer> w) { return make_ptr<teeReader>(std::move(r), std::move(w)); }

}  // namespace gopp::io
