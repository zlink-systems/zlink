package systems.zlink.tutorial.server

import jakarta.servlet.FilterChain
import jakarta.servlet.http.HttpServletRequest
import jakarta.servlet.http.HttpServletResponse
import java.nio.charset.StandardCharsets
import java.util.Base64
import org.springframework.stereotype.Component
import org.springframework.web.filter.OncePerRequestFilter

@Component
class TutorialAdminAuthentication : OncePerRequestFilter() {

    override fun shouldNotFilter(request: HttpServletRequest): Boolean =
        !request.servletPath.startsWith("/admin/")

    override fun doFilterInternal(
        request: HttpServletRequest,
        response: HttpServletResponse,
        filterChain: FilterChain,
    ) {
        val authorization = request.getHeader("Authorization")
        if (!isTutorialCredential(authorization)) {
            response.status = HttpServletResponse.SC_UNAUTHORIZED
            response.setHeader("WWW-Authenticate", "Basic realm=\"tutorial-admin\"")
            return
        }
        filterChain.doFilter(request, response)
    }

    private fun isTutorialCredential(authorization: String?): Boolean {
        if (authorization == null || !authorization.startsWith("Basic ", ignoreCase = true)) {
            return false
        }
        return try {
            val credentials =
                String(
                    Base64.getDecoder().decode(authorization.substring(6)),
                    StandardCharsets.UTF_8,
                )
            // Credentials are hard-coded because this is a self-contained tutorial, not a deployed
            // service.
            credentials == "ops:tutorial-admin"
        } catch (_: IllegalArgumentException) {
            false
        }
    }
}
