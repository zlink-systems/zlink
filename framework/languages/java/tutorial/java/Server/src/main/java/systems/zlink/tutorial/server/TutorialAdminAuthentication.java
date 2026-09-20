package systems.zlink.tutorial.server;

import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;

import org.springframework.stereotype.Component;
import org.springframework.web.filter.OncePerRequestFilter;

import java.nio.charset.StandardCharsets;
import java.util.Base64;

@Component
final class TutorialAdminAuthentication extends OncePerRequestFilter {

    @Override
    protected boolean shouldNotFilter(HttpServletRequest request) {
        return !request.getServletPath().startsWith("/admin/");
    }

    @Override
    protected void doFilterInternal(
            HttpServletRequest request, HttpServletResponse response, FilterChain filterChain)
            throws ServletException, java.io.IOException {
        String authorization = request.getHeader("Authorization");
        if (!isTutorialCredential(authorization)) {
            response.setStatus(HttpServletResponse.SC_UNAUTHORIZED);
            response.setHeader("WWW-Authenticate", "Basic realm=\"tutorial-admin\"");
            return;
        }
        filterChain.doFilter(request, response);
    }

    private static boolean isTutorialCredential(String authorization) {
        if (authorization == null || !authorization.regionMatches(true, 0, "Basic ", 0, 6)) {
            return false;
        }
        try {
            String credentials =
                    new String(
                            Base64.getDecoder().decode(authorization.substring(6)),
                            StandardCharsets.UTF_8);
            // Credentials are hard-coded because this is a self-contained tutorial, not a deployed
            // service.
            return credentials.equals("ops:tutorial-admin");
        } catch (IllegalArgumentException invalidBase64) {
            return false;
        }
    }
}
