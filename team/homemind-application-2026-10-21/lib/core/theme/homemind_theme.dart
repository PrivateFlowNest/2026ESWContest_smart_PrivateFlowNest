import 'package:flutter/material.dart';

abstract final class HomeMindColors {
  static const background = Color(0xFF020B16);
  static const surface = Color(0xFF071321);
  static const surfaceRaised = Color(0xFF0B1929);
  static const border = Color(0xFF1B3045);
  static const mint = Color(0xFF1FE1D2);
  static const purple = Color(0xFFA768FF);
  static const amber = Color(0xFFFFB000);
  static const green = Color(0xFF53D769);
  static const red = Color(0xFFFF4D57);
  static const blue = Color(0xFF31B7FF);
  static const textPrimary = Color(0xFFF4F7FB);
  static const textSecondary = Color(0xFFAAB5C3);
}

ThemeData buildHomeMindTheme() {
  final colorScheme = ColorScheme.fromSeed(
    seedColor: HomeMindColors.mint,
    brightness: Brightness.dark,
    surface: HomeMindColors.surface,
  );

  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    scaffoldBackgroundColor: HomeMindColors.background,
    colorScheme: colorScheme,
    fontFamilyFallback: const ['Noto Sans KR', 'Malgun Gothic', 'sans-serif'],
    textTheme: const TextTheme(
      displaySmall: TextStyle(
        color: HomeMindColors.textPrimary,
        fontSize: 34,
        fontWeight: FontWeight.w800,
        letterSpacing: -1.2,
      ),
      headlineMedium: TextStyle(
        color: HomeMindColors.textPrimary,
        fontSize: 25,
        fontWeight: FontWeight.w800,
        letterSpacing: -0.7,
      ),
      titleLarge: TextStyle(
        color: HomeMindColors.textPrimary,
        fontSize: 20,
        fontWeight: FontWeight.w700,
      ),
      titleMedium: TextStyle(
        color: HomeMindColors.textPrimary,
        fontSize: 16,
        fontWeight: FontWeight.w700,
      ),
      bodyLarge: TextStyle(
        color: HomeMindColors.textPrimary,
        fontSize: 16,
        height: 1.45,
      ),
      bodyMedium: TextStyle(
        color: HomeMindColors.textSecondary,
        fontSize: 14,
        height: 1.45,
      ),
    ),
    navigationBarTheme: NavigationBarThemeData(
      height: 72,
      backgroundColor: HomeMindColors.surface,
      indicatorColor: HomeMindColors.mint.withValues(alpha: 0.15),
      labelTextStyle: WidgetStateProperty.resolveWith((states) {
        return TextStyle(
          color: states.contains(WidgetState.selected)
              ? HomeMindColors.mint
              : HomeMindColors.textSecondary,
          fontWeight: states.contains(WidgetState.selected)
              ? FontWeight.w700
              : FontWeight.w500,
        );
      }),
      iconTheme: WidgetStateProperty.resolveWith((states) {
        return IconThemeData(
          color: states.contains(WidgetState.selected)
              ? HomeMindColors.mint
              : HomeMindColors.textSecondary,
        );
      }),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: HomeMindColors.surfaceRaised,
      hintStyle: const TextStyle(color: HomeMindColors.textSecondary),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(14),
        borderSide: const BorderSide(color: HomeMindColors.border),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(14),
        borderSide: const BorderSide(color: HomeMindColors.border),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(14),
        borderSide: const BorderSide(color: HomeMindColors.mint, width: 1.5),
      ),
    ),
  );
}
