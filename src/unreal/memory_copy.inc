bool copy_local_guarded(const void* source, void* destination, std::size_t size) noexcept {
    __try { std::memcpy(destination, source, size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safe_copy(const void* source, void* destination, std::size_t size) {
    if (!source || !destination || size == 0 || !is_readable_range(source, size)) return false;
    return copy_local_guarded(source, destination, size);
}
