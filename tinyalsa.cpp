#include <tinyalsa.hpp>

#include <algorithm>
#include <new>
#include <ostream>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace tinyalsa {

//==================================//
// Section: Native ALSA to TinyALSA //
//==================================//

namespace {

auto to_tinyalsa_class(int native_class_) noexcept
{
  switch (native_class_) {
    case SNDRV_PCM_CLASS_GENERIC:
      return pcm_class::generic;
    case SNDRV_PCM_CLASS_MULTI:
      return pcm_class::multi_channel;
    case SNDRV_PCM_CLASS_MODEM:
      return pcm_class::modem;
    case SNDRV_PCM_CLASS_DIGITIZER:
      return pcm_class::digitizer;
    default:
      break;
  }

  return pcm_class::unknown;
}

auto to_tinyalsa_subclass(int native_subclass) noexcept
{
  switch (native_subclass) {
    case SNDRV_PCM_SUBCLASS_GENERIC_MIX:
      return pcm_subclass::generic_mix;
    case SNDRV_PCM_SUBCLASS_MULTI_MIX:
      return pcm_subclass::multi_channel_mix;
    default:
      break;
  }

  return pcm_subclass::unknown;
}

tinyalsa::pcm_info to_tinyalsa_info(const snd_pcm_info& native_info) noexcept
{
  tinyalsa::pcm_info out;

  out.device               = native_info.device;
  out.subdevice            = native_info.subdevice;
  out.card                 = native_info.card;
  out.subdevices_count     = native_info.subdevices_count;
  out.subdevices_available = native_info.subdevices_avail;

  memcpy(out.id,      native_info.id,      std::min(sizeof(out.id),      sizeof(native_info.id)));
  memcpy(out.name,    native_info.name,    std::min(sizeof(out.name),    sizeof(native_info.name)));
  memcpy(out.subname, native_info.subname, std::min(sizeof(out.subname), sizeof(native_info.subname)));

  out.class_   = to_tinyalsa_class(native_info.dev_class);
  out.subclass = to_tinyalsa_subclass(native_info.dev_subclass);

  return out;
}

} // namespace

const char* get_error_description(int error) noexcept
{
  if (error == 0) {
    return "Success";
  } else {
    return ::strerror(error);
  }
}

//=============================//
// Section: Interleaved Reader //
//=============================//

result interleaved_pcm_reader::open(size_type card, size_type device, bool non_blocking) noexcept
{
  return pcm::open_capture_device(card, device, non_blocking);
}

generic_result<size_type> interleaved_pcm_reader::read_unformatted(void* frames, size_type frame_count) noexcept
{
  snd_xferi transfer {
    0 /* result */,
    frames,
    snd_pcm_uframes_t(frame_count),
  };

  auto err = ioctl(get_file_descriptor(), SNDRV_PCM_IOCTL_READI_FRAMES, &transfer);

  if (err != 0) {
    return { EINVAL, 0 };
  }

  return { 0, size_type(transfer.result) };
}

//==============//
// Section: PCM //
//==============//

/// Contains all the implementation data for a PCM.
class pcm_impl final
{
  friend pcm;
  /// The file descriptor for the opened PCM.
  int fd = invalid_fd();
  /// Opens a PCM by a specified path.
  ///
  /// @param path The path of the PCM to open.
  ///
  /// @param non_blocking Whether or not the call to ::open
  /// should block if the device is not available.
  ///
  /// @return On success, zero is returned.
  /// On failure, a copy of errno is returned.
  result open_by_path(const char* path, bool non_blocking) noexcept;
};

namespace {

/// This function allocates an instance
/// of the implementation data, if it is not null.
///
/// @param impl The current pointer to the implementation data.
/// If this is null then a new one is allocated.
///
/// @return A pointer to either an existing implementation
/// data instance, a new implementation data instance, or
/// in the case of a memory allocation failure a null pointer.
pcm_impl* lazy_init(pcm_impl* impl) noexcept
{
  if (impl) {
    return impl;
  }

  return new (std::nothrow) pcm_impl();
}

} // namespace

pcm::pcm() noexcept : self(nullptr) { }

pcm::pcm(pcm&& other) noexcept : self(other.self)
{
  other.self = nullptr;
}

pcm::~pcm()
{
  close();
  delete self;
}

int pcm::close() noexcept
{
  if (!self) {
    return 0;
  }

  if (self->fd != invalid_fd()) {

    auto result = ::close(self->fd);

    self->fd = invalid_fd();

    if (result == -1) {
      return errno;
    }
  }

  return 0;
}

int pcm::get_file_descriptor() const noexcept
{
  return self ? self->fd : invalid_fd();
}

bool pcm::is_open() const noexcept
{
  if (!self) {
    return false;
  }

  return self->fd != invalid_fd();
}

result pcm::prepare() noexcept
{
  if (!self) {
    return ENOENT;
  }

  auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_PREPARE);
  if (err < 0) {
    return errno;
  }

  return result();
}

result pcm::setup(const pcm_config&) noexcept
{
  return EPROTO;
}

result pcm::start() noexcept
{
  if (!self) {
    return ENOENT;
  }

  auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_START);
  if (err < 0) {
    return errno;
  }

  return result();
}

result pcm::drop() noexcept
{
  if (!self) {
    return ENOENT;
  }

  auto err = ::ioctl(self->fd, SNDRV_PCM_IOCTL_DROP);
  if (err < 0) {
    return errno;
  }

  return result();
}

generic_result<pcm_info> pcm::get_info() const noexcept
{
  using result_type = generic_result<pcm_info>;

  if (!self) {
    return result_type { ENOENT };
  }

  snd_pcm_info native_info;

  int err = ioctl(self->fd, SNDRV_PCM_IOCTL_INFO, &native_info);
  if (err != 0) {
    return result_type { errno };
  }

  return result_type { 0, to_tinyalsa_info(native_info) };
}

result pcm::open_capture_device(size_type card, size_type device, bool non_blocking) noexcept
{
  self = lazy_init(self);
  if (!self) {
    return result { ENOMEM };
  }

  char path[256];

  snprintf(path, sizeof(path), "/dev/snd/pcmC%luD%luc",
           (unsigned long) card,
           (unsigned long) device);

  return self->open_by_path(path, non_blocking);
}

result pcm::open_playback_device(size_type card, size_type device, bool non_blocking) noexcept
{
  self = lazy_init(self);
  if (!self) {
    return result { ENOMEM };
  }

  char path[256];

  snprintf(path, sizeof(path), "/dev/snd/pcmC%luD%lup",
           (unsigned long) card,
           (unsigned long) device);

  return self->open_by_path(path, non_blocking);
}

result pcm_impl::open_by_path(const char* path, bool non_blocking) noexcept
{
  if (fd != invalid_fd()) {
    ::close(fd);
  }

  fd = ::open(path, non_blocking ? (O_RDWR | O_NONBLOCK) : O_RDWR);
  if (fd < 0) {
    fd = invalid_fd();
    return result { errno };
  } else {
    return result { 0 };
  }
}

//===================//
// Section: PCM list //
//===================//

namespace {

/// A wrapper around a directory pointer
/// that closes the directory when it goes out of scope.
struct dir_wrapper final
{
  /// A pointer to the opened directory.
  DIR* ptr = nullptr;
  /// Casts the wrapper to a directory pointer.
  ///
  /// @return The directory pointer.
  inline operator DIR* () noexcept
  {
    return ptr;
  }
  /// Closes the directory if it was opened.
  ~dir_wrapper()
  {
    if (ptr) {
      closedir(ptr);
    }
  }
  /// Opens a directory.
  ///
  /// @param path The path of the directory to open.
  ///
  /// @return True on success, false on failure.
  bool open(const char* path) noexcept
  {
    ptr = opendir(path);
    return !!ptr;
  }
};

/// Represents a PCM name that was parsed
/// from a directory entry.
struct parsed_name final
{
  /// Whether or not the name is valid.
  bool valid = false;
  /// The index of the card.
  size_type card = 0;
  /// The index of the device.
  size_type device = 0;
  /// Whether or not it's a capture PCM.
  bool is_capture = false;
  /// Constructs a new parsed name instance.
  ///
  /// @param name A pointer to the name to parse.
  constexpr parsed_name(const char* name) noexcept
  {
    valid = parse(name);
  }
private:
  /// Parses a name given to the constructor.
  ///
  /// @param name The filename to be parsed.
  ///
  /// @return True on a match, false on failure.
  constexpr bool parse(const char* name) noexcept;
  /// Indicates if a character is a decimal number or not.
  ///
  /// @return True if it is a decimal character, false otherwise.
  static constexpr bool is_dec(char c) noexcept
  {
    return (c >= '0') && (c <= '9');
  }
  /// Parses a decimal number.
  ///
  /// @param c The character to convert into a decimal number.
  ///
  /// @return The resultant decimal number.
  constexpr size_type to_dec(char c) noexcept
  {
    return size_type(c - '0');
  }
};

constexpr bool parsed_name::parse(const char* name) noexcept
{
  auto name_length = strlen(name);
  if (!name_length) {
    return false;
  }

  if ((name[0] != 'p')
   || (name[1] != 'c')
   || (name[2] != 'm')
   || (name[3] != 'C')) {
    return false;
  }

  size_type d_pos = name_length;

  for (size_type i = 4; i < name_length; i++) {
    if (name[i] == 'D') {
      d_pos = i;
      break;
    }
  }

  if (d_pos >= name_length) {
    return false;
  }

  if (name[name_length - 1] == 'c') {
    is_capture = true;
  } else if (name[name_length - 1] == 'p') {
    is_capture = false;
  } else {
    return false;
  }

  device = 0;
  card = 0;

  for (size_type i = 4; i < d_pos; i++) {
    if (!is_dec(name[i])) {
      return false;
    }
    card *= 10;
    card += to_dec(name[i]);
  }

  for (size_type i = d_pos + 1; i < (name_length - 1); i++) {
    if (!is_dec(name[i])) {
      return false;
    }
    device *= 10;
    device += to_dec(name[i]);
  }

  return true;
}

} // namespace

class pcm_list_impl final
{
  friend pcm_list;
  /// The array of information instances.
  std::vector<pcm_info> info_vec;
};

pcm_list::pcm_list() noexcept : self(nullptr)
{
  self = new (std::nothrow) pcm_list_impl();
  if (!self) {
    return;
  }

  dir_wrapper snd_dir;

  if (!snd_dir.open("/dev/snd")) {
    return;
  }

  dirent* entry = nullptr;

  for (;;) {

    entry = readdir(snd_dir);
    if (!entry) {
      break;
    }

    parsed_name name(entry->d_name);
    if (!name.valid) {
      continue;
    }

    result open_result;

    pcm p;

    if (name.is_capture) {
      open_result = p.open_capture_device(name.card, name.device);
    } else {
      open_result = p.open_playback_device(name.card, name.device);
    }

    if (open_result.failed()) {
      continue;
    }

    auto info_result = p.get_info();
    if (info_result.failed()) {
      continue;
    }

    try {
      self->info_vec.emplace_back(info_result.unwrap());
    } catch (...) {
      break;
    }
  }
}

pcm_list::pcm_list(pcm_list&& other) noexcept : self(other.self)
{
  other.self = nullptr;
}

pcm_list::~pcm_list()
{
  delete self;
}

const pcm_info* pcm_list::data() const noexcept
{
  return self ? self->info_vec.data() : nullptr;
}

size_type pcm_list::size() const noexcept
{
  return self ? self->info_vec.size() : 0;
}

//============================//
// Section: Stream Operations //
//============================//

namespace detail {

std::ostream& write_error(std::ostream& output, int error)
{
  return output << get_error_description(error);
}

} // namespace detail

std::ostream& operator << (std::ostream& output, const result& res)
{
  return output << res.error_description();
}

std::ostream& operator << (std::ostream& output, pcm_class class_)
{
  switch (class_) {
    case pcm_class::unknown:
      output << "Unknown";
      break;
    case pcm_class::generic:
      output << "Generic";
      break;
    case pcm_class::multi_channel:
      output << "Multi-channel";
      break;
    case pcm_class::modem:
      output << "Modem";
      break;
    case pcm_class::digitizer:
      output << "Digitizer";
      break;
  }

  return output;
}

std::ostream& operator << (std::ostream& output, pcm_subclass subclass)
{
  switch (subclass) {
    case pcm_subclass::unknown:
      output << "Unknown";
      break;
    case pcm_subclass::generic_mix:
      output << "Generic Mix";
      break;
    case pcm_subclass::multi_channel_mix:
      output << "Multi-channel Mix";
      break;
  }

  return output;
}

std::ostream& operator << (std::ostream& output, const pcm_info& info)
{
  output << "card      : " << info.card << std::endl;
  output << "device    : " << info.device << std::endl;
  output << "subdevice : " << info.subdevice << std::endl;
  output << "class     : " << info.class_ << std::endl;
  output << "subclass  : " << info.subclass << std::endl;
  output << "id        : " << info.id << std::endl;
  output << "name:     : " << info.name << std::endl;
  output << "subname   : " << info.subname << std::endl;
  output << "subdevices count     : " << info.subdevices_count << std::endl;
  output << "subdevices available : " << info.subdevices_available << std::endl;
  return output;
}

} // namespace tinyalsa