module private_import.main

use private_import.models.Hidden

function invalid(value: Hidden) -> Int64 effects { pure } {
    return value.value
}
