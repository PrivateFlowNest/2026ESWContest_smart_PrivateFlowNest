import 'dart:convert';
import 'dart:io';

/// `<repo>/contracts/pfn.v1` (read only), relative to the package root where
/// `flutter test` runs.
Map<String, dynamic> contractJson(String path) => Map<String, dynamic>.from(
    jsonDecode(File('../../../contracts/pfn.v1/$path').readAsStringSync())
        as Map);

final _uuid = RegExp(
    r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$');

/// Checks [value] against the JSON Schema keywords the pfn.v1 message schemas
/// use (type, const, enum, format uuid, pattern, min/maxLength, minimum,
/// required, properties, additionalProperties false, items, maxItems, anyOf,
/// allOf, not, if/then/else). Any other keyword throws, so a schema change can
/// never pass here unchecked.
List<String> schemaErrors(Map<String, dynamic> schema, Object? value,
    [String at = r'$']) {
  Map<String, dynamic> sub(Object? rule) =>
      Map<String, dynamic>.from(rule as Map);
  final errors = <String>[];
  for (final MapEntry(:key, value: rule) in schema.entries) {
    switch (key) {
      case r'$schema' || r'$id' || 'title' || 'description':
        break;
      case 'type':
        final ok = switch (rule) {
          'object' => value is Map,
          'string' => value is String,
          'integer' => value is int,
          'boolean' => value is bool,
          'array' => value is List,
          'null' => value == null,
          _ => throw UnsupportedError('type $rule'),
        };
        if (!ok) errors.add('$at: is not $rule');
      case 'pattern':
        if (value is String && !RegExp(rule as String).hasMatch(value)) {
          errors.add('$at: does not match $rule');
        }
      case 'maxItems':
        if (value is List && value.length > (rule as int)) {
          errors.add('$at: more than $rule items');
        }
      case 'items':
        if (value is List) {
          for (var i = 0; i < value.length; i++) {
            errors.addAll(schemaErrors(sub(rule), value[i], '$at[$i]'));
          }
        }
      case 'anyOf':
        if ((rule as List)
            .every((option) => schemaErrors(sub(option), value, at).isNotEmpty)) {
          errors.add('$at: matches no anyOf option');
        }
      case 'allOf':
        for (final part in rule as List) {
          errors.addAll(schemaErrors(sub(part), value, at));
        }
      case 'not':
        if (schemaErrors(sub(rule), value, at).isEmpty) {
          errors.add('$at: matches a "not" schema');
        }
      case 'if':
        final branch =
            schema[schemaErrors(sub(rule), value, at).isEmpty ? 'then' : 'else'];
        if (branch != null) errors.addAll(schemaErrors(sub(branch), value, at));
      case 'then' || 'else':
        if (!schema.containsKey('if')) throw UnsupportedError('$key without if');
      case 'const':
        if (value != rule) errors.add('$at: must be $rule');
      case 'enum':
        if (!(rule as List).contains(value)) errors.add('$at: not in $rule');
      case 'format':
        if (rule != 'uuid') throw UnsupportedError('format $rule');
        if (value is String && !_uuid.hasMatch(value)) {
          errors.add('$at: is not a uuid');
        }
      case 'minLength':
        if (value is String && value.runes.length < (rule as int)) {
          errors.add('$at: too short');
        }
      case 'maxLength':
        if (value is String && value.runes.length > (rule as int)) {
          errors.add('$at: too long');
        }
      case 'minimum':
        if (value is num && value < (rule as num)) {
          errors.add('$at: below $rule');
        }
      case 'required':
        for (final name in rule as List) {
          if (value is Map && !value.containsKey(name)) {
            errors.add('$at.$name: is required');
          }
        }
      case 'additionalProperties':
        if (rule != false) throw UnsupportedError('additionalProperties $rule');
        final known = (schema['properties'] as Map?) ?? const {};
        if (value is Map) {
          for (final name in value.keys) {
            if (!known.containsKey(name)) errors.add('$at.$name: not allowed');
          }
        }
      case 'properties':
        if (value is Map) {
          for (final property in (rule as Map).entries) {
            if (!value.containsKey(property.key)) continue;
            errors.addAll(schemaErrors(
                Map<String, dynamic>.from(property.value as Map),
                value[property.key],
                '$at.${property.key}'));
          }
        }
      default:
        throw UnsupportedError('schema keyword "$key" is not checked here');
    }
  }
  return errors;
}
