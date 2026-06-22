# Sprint M — Route Extraction Discovery

Date: 2026-06-23

## Current State

| Metric | Value |
|--------|------:|
| `Route::*` calls in source | 3,425 |
| Route files | 4 (web.php, api.php, channels.php, console.php) |
| Controller classes | 578 |
| **CBM Routes extracted** | **44** |
| **CBM ROUTES_TO edges** | **8** |
| **Coverage** | **1.3%** |

## Route Pattern Inventory

| Pattern | Count | Resolvable? |
|---------|------:|:-----------:|
| `Route::post()` | 2,229 | ✅ Static string path |
| `Route::get()` | 714 | ✅ Static string path |
| `Route::prefix()` | 138 | 🟡 Group modifier |
| `Route::middleware()` | 115 | 🟡 Attribute, not route |
| `Route::group()` | 78 | 🟡 Nested routes |
| `Route::delete()` | 6 | ✅ |
| `Route::any()` | 5 | ✅ |
| `Route::match()` | 3 | ✅ |
| `Route::patch()` | 1 | ✅ |
| `Route::fallback()` | 3 | ✅ |

## Route Handler Patterns

```php
// Pattern A: Controller class array (most common in Laravel 8+)
Route::post('/deposit', [DepositController::class, 'store'])

// Pattern B: Controller string
Route::post('/deposit', 'DepositController@store')

// Pattern C: Closure
Route::get('/health', function() { return 'ok'; })

// Pattern D: Resource
Route::resource('members', MemberController::class)
```

## Why CBM misses 98.7%

1. **Route files in packages/** — CBM checks `routes/` only, but 1168lot has routes in
   `packages/Gametech/*/routes/` or defined via ServiceProvider::boot()

2. **Controller array syntax** — `[Class::class, 'method']` not parsed for method reference

3. **Closure routes** — anonymous functions not extracted as Route targets

4. **Route::group nesting** — prefix/domain/middleware groups wrap actual route definitions

## Estimated Impact

```
After fix:
  Routes:           44 → ~2,000+ (est. 60% coverage of 3,425 calls)
  ROUTES_TO edges:   8 → ~1,200+ (Route → Controller.method)
  Controller methods with inbound ROUTES_TO: 0 → 500+
```

## Implementation Notes

- Parse `routes/*.php` AND `packages/*/routes/*.php`
- Handle `[ClassName::class, 'methodName']` controller arrays
- Extract HTTP method from `Route::post/get/delete/etc`
- Skip closure routes (no traceable target)
- Follow `Route::group()` nesting for prefix/middleware context
