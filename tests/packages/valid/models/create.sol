module example.models

public function create(value: Int64) -> Counter effects { pure }
ensures { result.value == value }
{
    return Counter { value = normalize(value) }
}
