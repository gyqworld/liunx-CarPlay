#pragma once

#include <string>

namespace carplay {

/** Persistent paths for settings, pairing identity, diagnostics. */
class IStorage {
 public:
  virtual ~IStorage() = default;
  virtual std::string data_dir() const = 0;
  virtual bool ensure_data_dir() const = 0;
};

class PosixStorage final : public IStorage {
 public:
  explicit PosixStorage(std::string root = {});
  std::string data_dir() const override;
  bool ensure_data_dir() const override;

 private:
  std::string root_;
};

}  // namespace carplay
