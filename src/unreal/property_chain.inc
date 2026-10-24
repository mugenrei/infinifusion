template<class Visitor>
bool visit_property_names(const void* structure, Visitor&& visitor) {
    std::vector<const void*> hierarchy;
    std::unordered_set<const void*> types;
    for (auto* current=structure; current;) {
        if(hierarchy.size()>=128 || !types.insert(current).second) return false;
        hierarchy.push_back(current);
        const auto next=safe_read_at<void*>(current,0x40);
        if(!next) return false;
        current=*next;
    }
    std::unordered_set<void*> fields;

    for(auto type=hierarchy.rbegin();type!=hierarchy.rend();++type) {
        const auto first=safe_read_at<void*>(*type,0x50);
        if(!first) return false;
        for(auto* field=*first;field;) {
            if(fields.size()>=8192 || !fields.insert(field).second) return false;
            const auto header=safe_read<std::array<std::uint8_t,0x30>>(field);
            if(!header) return false;
            void* next{}; FName name{};
            std::memcpy(&next,header->data()+0x20,sizeof(next));
            std::memcpy(&name,header->data()+0x28,sizeof(name));
            if(!visitor(field,name)) return true;
            field=next;
        }
    }
    return true;
}
