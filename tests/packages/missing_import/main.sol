module missing_import.main

use missing_import.unknown.Value

function invalid() -> Int64 effects { pure } {
    return 0
}
