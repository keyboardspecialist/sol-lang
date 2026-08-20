module relocation.seed

record SeedPair { value: Int64 }
enum SeedChoice { some(value: Int64), none }
capability SeedRead { function read() -> Int64 effects { seed.read<Self> } }

function seed_identity<T>(value: T) -> T { return value }
function seed(value: Option<Int64>, choice: SeedChoice) -> Option<Int64>
    ensures { result == old(result) } {
    let pair = SeedPair { value = 1 }
    let selected = match choice { some(item) => item none => 0 }
    let item = value?
    return some(seed_identity<Int64>(pair.value + selected + item))
}
